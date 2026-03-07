#pragma once

#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/IChunk.h"
#include "world/level/block/blocks/DoublePlantBlock.h"
#include "world/level/block/blocks/SculkVeinBlock.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/feature/BlockChangeTrace.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include "util/IntProvider.h"
#include "util/JavaHashSet.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "world/level/block/SculkSpreader.h"
#include "world/level/chunk/BulkSectionAccess.h"
#include "core/SectionPos.h"
#include "math/Mth.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <array>
#include <cmath>
#include <iostream>

// Alias for Direction in the core namespace for backwards compatibility
namespace minecraft { namespace core { using Direction = levelgen::blockpredicates::Direction; } }

// Reference: net/minecraft/world/level/levelgen/feature/Feature.java
// Reference: net/minecraft/world/level/levelgen/feature/ConfiguredFeature.java
// Reference: net/minecraft/world/level/levelgen/feature/FeaturePlaceContext.java

namespace minecraft {

// Forward declaration (WorldGenLevel is now in levelgen namespace)
namespace levelgen {
    class WorldGenLevel;
}

// Forward declaration for PlacedFeature (in placement namespace)
namespace levelgen {
namespace placement {
    class PlacedFeature;
}
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
    WorldGenLevel* m_level;
    ChunkGenerator* m_chunkGenerator;
    WorldgenRandom* m_random;
    core::BlockPos m_origin;
    const FC& m_config;

public:
    FeaturePlaceContext(
        std::optional<void*> topFeature,
        WorldGenLevel* level,
        ChunkGenerator* chunkGenerator,
        WorldgenRandom* random,
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
    WorldGenLevel* level() const { return m_level; }
    ChunkGenerator* chunkGenerator() const { return m_chunkGenerator; }
    WorldgenRandom& random() const { return *m_random; }
    const core::BlockPos& origin() const { return m_origin; }
    const FC& config() const { return m_config; }

    /**
     * Get the chunk for the origin position
     * Convenience method for features that still need direct chunk access
     * Eventually features should transition to using WorldGenLevel methods
     */
    ::world::IChunk* getChunkForOrigin() const {
        int chunkX = m_origin.getX() >> 4;
        int chunkZ = m_origin.getZ() >> 4;
        return const_cast<WorldGenLevel*>(m_level)->getChunk(chunkX, chunkZ);
    }
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
    if (!state) return false;
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
    if (!state) return false;
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
        if (!state) return false;
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
        WorldGenLevel* level,
        ChunkGenerator* chunkGenerator,
        WorldgenRandom& random,
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
    static bool isGrassOrDirt(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* block = level->getBlockState(pos);
        if (!block) return false;
        BlockState* state = static_cast<BlockState*>(block);
        return FeatureHelpers::isDirt(state);
    }

    /**
     * Check if any neighbor is air
     * Reference: Feature.java lines 183-185
     *
     * Takes a block getter function to allow cross-chunk access via BulkSectionAccess.
     * This matches Java's signature: isAdjacentToAir(Function<BlockPos, BlockState>, BlockPos)
     *
     * @param blockGetter Function to get block state at any position
     * @param pos Position to check neighbors of
     */
    static bool isAdjacentToAir(
        std::function<BlockState*(const core::BlockPos&)> blockGetter,
        const core::BlockPos& pos
    ) {
        for (const auto& [dx, dy, dz] : DIRECTION_OFFSETS) {
            core::BlockPos neighborPos(pos.getX() + dx, pos.getY() + dy, pos.getZ() + dz);
            BlockState* block = blockGetter(neighborPos);
            if (block && block->isAir()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Check if any neighbor is air (WorldGenLevel overload for convenience)
     * Uses WorldGenLevel::getBlockState which handles coordinate translation.
     * For cross-chunk access, use the function-based overload with BulkSectionAccess.
     */
    static bool isAdjacentToAir(WorldGenLevel* level, const core::BlockPos& pos) {
        auto blockGetter = [level](const core::BlockPos& p) -> BlockState* {
            return level->getBlockState(p);
        };
        return isAdjacentToAir(blockGetter, pos);
    }

protected:
    /**
     * Set a block at position
     * Reference: Feature.java lines 137-139
     */
    void setBlock(WorldGenLevel* level, const core::BlockPos& pos, BlockState* state) {
        level->setBlock(pos, state, 2);
    }

    /**
     * Safely set a block at position if predicate matches
     * Reference: Feature.java lines 145-150
     */
    void safeSetBlock(
        WorldGenLevel* level,
        const core::BlockPos& pos,
        BlockState* state,
        std::function<bool(BlockState*)> canReplace
    ) {
        BlockState* existingBlock = level->getBlockState(pos);
        if (existingBlock) {
            BlockState* existingState = static_cast<BlockState*>(existingBlock);
            if (canReplace(existingState)) {
                level->setBlock(pos, state, 2);
            }
        }
    }

    /**
     * Mark positions above for post-processing
     * Reference: Feature.java lines 187-199
     */
    void markAboveForPostProcessing(WorldGenLevel* level, const core::BlockPos& placePos) {
        core::BlockPos::MutableBlockPos pos(placePos.getX(), placePos.getY(), placePos.getZ());

        for (int i = 0; i < 2; ++i) {
            pos.move(0, 1, 0);
            BlockState* block = level->getBlockState(pos);
            if (block) {
                BlockState* state = static_cast<BlockState*>(block);
                if (state->isAir()) {
                    return;
                }
            }
            // markPosForPostprocessing needs IChunk access - get chunk from level
            int chunkX = pos.getX() >> 4;
            int chunkZ = pos.getZ() >> 4;
            ::world::IChunk* chunk = level->getChunk(chunkX, chunkZ);
            if (chunk) chunk->markPosForPostprocessing(pos);
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
     * Reference: ConfiguredFeature.java lines 22-24
     */
    virtual bool place(
        WorldGenLevel* level,
        ChunkGenerator* chunkGenerator,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        ChunkGenerator* chunkGenerator,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        // Construct context and call virtual place() method directly
        FeaturePlaceContext<FC> ctx(std::nullopt, level, chunkGenerator, &random, origin, m_config);
        return m_feature->place(ctx);
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
 * SimpleBlockConfiguration - Configuration for simple block placement
 * Reference: SimpleBlockConfiguration.java
 *
 * Uses BlockStateProvider to allow weighted random state selection
 */
class SimpleBlockConfiguration : public FeatureConfiguration {
public:
    // Reference: SimpleBlockConfiguration.java record fields
    feature::stateproviders::BlockStateProvider* toPlace;
    bool scheduleTick;

    // Constructor matching Java record SimpleBlockConfiguration(BlockStateProvider toPlace, boolean scheduleTick)
    SimpleBlockConfiguration(feature::stateproviders::BlockStateProvider* provider, bool schedule = false)
        : toPlace(provider)
        , scheduleTick(schedule)
        , m_legacyState(nullptr)
    {}

    // Legacy constructor for compatibility with code using BlockState* directly
    explicit SimpleBlockConfiguration(BlockState* block)
        : toPlace(nullptr)
        , scheduleTick(false)
        , m_legacyState(block)
    {}

    // Get state - uses provider or legacy state
    BlockState* getState(WorldgenRandom& random, const core::BlockPos& pos) const {
        if (toPlace) {
            return toPlace->getState(random, pos);
        }
        return m_legacyState;
    }

private:
    BlockState* m_legacyState;
};

/**
 * SimpleBlockFeature - Places a single block
 * Reference: SimpleBlockFeature.java
 */
class SimpleBlockFeature : public Feature<SimpleBlockConfiguration> {
public:
    // Reference: SimpleBlockFeature.java place() lines 16-41
    bool place(FeaturePlaceContext<SimpleBlockConfiguration>& context) override {
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& random = context.random();

        BlockState* state = context.config().getState(random, pos);
        if (!state) {
            return false;
        }

        if (!state->canSurvive(*context.level(), pos)) {
            return false;
        }

        if (auto* doublePlantBlock = dynamic_cast<world::level::block::DoublePlantBlock*>(state->getBlock())) {
            if (!context.level()->isEmptyBlock(pos.above())) {
                return false;
            }
            doublePlantBlock->placeAt(context.level(), state, pos, 2);
        } else {
            context.level()->setBlock(pos, state, 2);
        }

        if (context.config().scheduleTick) {
            BlockState* placedState = context.level()->getBlockState(pos);
            if (placedState && placedState->getBlock()) {
                context.level()->scheduleTick(pos, placedState->getBlock()->getIdentifier(), 1);
            }
        }

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
        BlockState* state;

        TargetBlockState(std::shared_ptr<structure::templatesystem::RuleTest> t, BlockState* s)
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
        BlockState* state
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
    // Bring base class place overloads into scope
    using Feature<OreConfiguration>::place;

    /**
     * Place ore vein
     * Reference: OreFeature.java place() lines 23-53
     */
    bool place(FeaturePlaceContext<OreConfiguration>& context) override {
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();
        const OreConfiguration& config = context.config();
        WorldGenLevel* worldGenLevel = context.level();

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

        // Reference: OreFeature.java lines 44-52
        // Check if any position in the vein area is below the terrain surface
        // This prevents ore from generating in the air
        for (int32_t xprobe = xStart; xprobe <= xStart + sizeXZ; ++xprobe) {
            for (int32_t zprobe = zStart; zprobe <= zStart + sizeXZ; ++zprobe) {
                if (yStart <= worldGenLevel->getHeight(Heightmap::Types::OCEAN_FLOOR_WG, xprobe, zprobe)) {
                    // Pass worldGenLevel to doPlace - it creates BulkSectionAccess internally
                    // Reference: OreFeature.java line 47
                    return doPlace(worldGenLevel, random, config, x0, x1, z0, z1, y0, y1, xStart, yStart, zStart, sizeXZ, sizeY);
                }
            }
        }

        return false;
    }

protected:
    /**
     * Perform ore placement
     * Reference: OreFeature.java doPlace() lines 55-152
     *
     * Uses BulkSectionAccess for cross-chunk block access, matching Java exactly.
     * When checking if an ore position is adjacent to air, the blockGetter function
     * goes through BulkSectionAccess which returns AIR for non-loaded chunks.
     */
    bool doPlace(
        WorldGenLevel* worldGenLevel,
        WorldgenRandom& random,
        const OreConfiguration& config,
        double x0, double x1,
        double z0, double z1,
        double y0, double y1,
        int32_t xStart, int32_t yStart, int32_t zStart,
        int32_t sizeXZ, int32_t sizeY
    ) {
        // Create BulkSectionAccess for cross-chunk block access
        // Reference: OreFeature.java line 95
        // try (BulkSectionAccess sectionGetter = new BulkSectionAccess(level))
        ::minecraft::world::level::chunk::BulkSectionAccess sections(worldGenLevel);

        // Create block getter function matching Java's sectionGetter::getBlockState
        // Reference: OreFeature.java line 132
        auto blockGetter = [&sections](const core::BlockPos& pos) -> BlockState* {
            return sections.getBlockState(pos);
        };

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
            // CRITICAL: Match Java's exact type conversions for parity
            // Java: ((double)(Mth.sin((double)((float)Math.PI * step)) + 1.0F) * ss + (double)1.0F) / (double)2.0F
            // Mth.sin returns float, addition with 1.0F is in float, THEN cast to double
            float sinResult = Mth::sin(static_cast<double>(static_cast<float>(M_PI) * step));
            double r = (static_cast<double>(sinResult + 1.0f) * ss + 1.0) / 2.0;
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

            // Use Mth::floor to match Java's Mth.floor() exactly
            // Reference: OreFeature.java lines 102-107
            int32_t xMin = std::max(Mth::floor(xx - r), xStart);
            int32_t yMin = std::max(Mth::floor(yy - r), yStart);
            int32_t zMin = std::max(Mth::floor(zz - r), zStart);
            int32_t xMax = std::max(Mth::floor(xx + r), xMin);
            int32_t yMax = std::max(Mth::floor(yy + r), yMin);
            int32_t zMax = std::max(Mth::floor(zz + r), zMin);

            for (int32_t x = xMin; x <= xMax; ++x) {
                double xd = (static_cast<double>(x) + 0.5 - xx) / r;
                if (xd * xd >= 1.0) continue;

                for (int32_t y = yMin; y <= yMax; ++y) {
                    double yd = (static_cast<double>(y) + 0.5 - yy) / r;
                    if (xd * xd + yd * yd >= 1.0) continue;

                    for (int32_t z = zMin; z <= zMax; ++z) {
                        double zd = (static_cast<double>(z) + 0.5 - zz) / r;
                        if (xd * xd + yd * yd + zd * zd >= 1.0) continue;

                        // Check if outside build height
                        // Reference: OreFeature.java line 117
                        if (worldGenLevel->isOutsideBuildHeight(core::BlockPos(x, y, z))) continue;

                        // Check if already tested
                        // Reference: OreFeature.java lines 118-120
                        int32_t bitSetIndex = (x - xStart) + (y - yStart) * sizeXZ + (z - zStart) * sizeXZ * sizeY;
                        if (bitSetIndex < 0 || bitSetIndex >= static_cast<int32_t>(tested.size())) continue;
                        if (tested[bitSetIndex]) continue;
                        tested[bitSetIndex] = true;

                        orePos.set(x, y, z);

                        // Check if we can write to this position
                        // Reference: OreFeature.java line 122
                        if (!worldGenLevel->ensureCanWrite(orePos)) {
                            continue;
                        }

                        // Get section via BulkSectionAccess
                        // Reference: OreFeature.java line 123
                        ::minecraft::world::LevelChunkSection* section = sections.getSection(orePos);
                        if (section == nullptr) {
                            // Non-loaded chunk or out of bounds - skip
                            // This is the KEY behavior that matches Java
                            continue;
                        }

                        // Get block state from section using section-relative coords
                        // Reference: OreFeature.java lines 125-128
                        int32_t relX = core::SectionPos::sectionRelative(x);
                        int32_t relY = core::SectionPos::sectionRelative(y);
                        int32_t relZ = core::SectionPos::sectionRelative(z);
                        BlockState* existingBlock = section->getBlockState(relX, relY, relZ);
                        if (!existingBlock) {
                            continue;
                        }

                        // Check each target
                        // Reference: OreFeature.java lines 130-137
                        for (const auto& targetState : config.targetStates) {
                            // Pass blockGetter function (not IChunk*)
                            // Reference: OreFeature.java line 132: sectionGetter::getBlockState
                            if (canPlaceOre(existingBlock, blockGetter, random, config, targetState, orePos)) {
                                // Trace block change
                                if (feature::BlockChangeTrace::enabled) {
                                    feature::BlockChangeTrace::log(x, y, z,
                                        existingBlock->getIdentifier(),
                                        targetState.state->getIdentifier());
                                }
                                // Set block using section-relative coords
                                // Reference: OreFeature.java line 133
                                section->setBlockState(relX, relY, relZ, targetState.state, false);
                                ++placed;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // BulkSectionAccess automatically closes via destructor (RAII)
        // Reference: OreFeature.java line 149 (try-with-resources auto-close)

        return placed > 0;
    }

    /**
     * Check if ore can be placed at position
     * Reference: OreFeature.java canPlaceOre() lines 154-162
     *
     * Takes a block getter function to allow cross-chunk access via BulkSectionAccess.
     * This matches Java's signature: canPlaceOre(BlockState, Function<BlockPos, BlockState>, ...)
     *
     * @param orePosState Current block state at ore position
     * @param blockGetter Function to get block state at any position (for neighbor checks)
     * @param random Random source
     * @param config Ore configuration
     * @param targetState Target block state configuration
     * @param orePos Position to place ore
     */
    static bool canPlaceOre(
        BlockState* orePosState,
        std::function<BlockState*(const core::BlockPos&)> blockGetter,
        WorldgenRandom& random,
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
        return !isAdjacentToAir(blockGetter, orePos);
    }

    /**
     * Check if air check should be skipped
     * Reference: OreFeature.java shouldSkipAirCheck() lines 164-172
     */
    static bool shouldSkipAirCheck(WorldgenRandom& random, float discardChanceOnAirExposure) {
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
    placement::PlacedFeature* feature;
    std::function<bool(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const core::BlockPos&)> featurePlacer;

    RandomPatchConfiguration(
        int tries = 128,
        int xzSpread = 7,
        int ySpread = 3,
        placement::PlacedFeature* feature = nullptr
    )
        : tries(tries)
        , xzSpread(xzSpread)
        , ySpread(ySpread)
        , feature(feature)
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
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();
        WorldGenLevel* level = context.level();
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

            if (config.feature) {
                if (config.feature->place(level, generator, random, grassPos)) {
                    ++placed;
                }
            } else if (config.featurePlacer && config.featurePlacer(level, generator, random, grassPos)) {
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();

        // Reference: SpringFeature.java lines 18-26
        // Check block above
        BlockState* aboveBlock = level->getBlockState(origin.above());
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
            BlockState* belowBlock = level->getBlockState(origin.below());
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
        BlockState* currentBlock = level->getBlockState(origin);
        if (currentBlock) {
            std::string currentName = currentBlock->getIdentifier();
            BlockState* currentState = static_cast<BlockState*>(minecraft::world::level::block::Blocks::getDefaultState(currentName));
            if (!currentState->isAir() && config.validBlocks.find(currentName) == config.validBlocks.end()) {
                return false;
            }
        }

        // Reference: SpringFeature.java lines 27-47
        // Count adjacent rock blocks
        int rockCount = 0;
        auto checkRock = [&](const core::BlockPos& pos) {
            BlockState* block = level->getBlockState(pos);
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
            BlockState* block = level->getBlockState(pos);
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
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();

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
                        BlockState* block = level->getBlockState(checkPos);
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
                        BlockState* existingBlock = level->getBlockState(placePos);
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
        return surface == CaveSurface::CEILING ? 1 : -1;
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
    placement::PlacedFeature* vegetationFeature;
    CaveSurface surface;
    std::shared_ptr<util::IntProvider> depth;
    float extraBottomBlockChance;
    int verticalRange;
    float vegetationChance;
    std::shared_ptr<util::IntProvider> xzRadius;
    float extraEdgeColumnChance;
    // Vegetation feature holder would go here
    std::function<bool(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const core::BlockPos&)> vegetationPlacer;

    VegetationPatchConfiguration(
        const std::string& replaceable,
        std::shared_ptr<feature::stateproviders::BlockStateProvider> groundState,
        placement::PlacedFeature* vegetationFeature,
        CaveSurface surface,
        std::shared_ptr<util::IntProvider> depth,
        float extraBottomBlockChance,
        int verticalRange,
        float vegetationChance,
        std::shared_ptr<util::IntProvider> xzRadius,
        float extraEdgeColumnChance
    )
        : replaceable(replaceable)
        , groundState(std::move(groundState))
        , vegetationFeature(vegetationFeature)
        , surface(surface)
        , depth(std::move(depth))
        , extraBottomBlockChance(extraBottomBlockChance)
        , verticalRange(verticalRange)
        , vegetationChance(vegetationChance)
        , xzRadius(std::move(xzRadius))
        , extraEdgeColumnChance(extraEdgeColumnChance)
        , vegetationPlacer(nullptr)
    {}

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
        , groundState(std::move(groundState))
        , vegetationFeature(nullptr)
        , surface(surface)
        , depth(std::move(depth))
        , extraBottomBlockChance(extraBottomBlockChance)
        , verticalRange(verticalRange)
        , vegetationChance(vegetationChance)
        , xzRadius(std::move(xzRadius))
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
        WorldGenLevel* level = context.level();
        const VegetationPatchConfiguration& config = context.config();
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();
        int xRadius = config.xzRadius->sample(random) + 1;
        int zRadius = config.xzRadius->sample(random) + 1;
        util::JavaHashSet<core::BlockPos> surface = placeGroundPatch(level, config, random, origin, xRadius, zRadius);
        distributeVegetation(context, level, config, random, surface, xRadius, zRadius);
        return !surface.empty();
    }

protected:
    /**
     * Place ground patch
     * Reference: VegetationPatchFeature.java placeGroundPatch() lines 35-76
     */
    virtual util::JavaHashSet<core::BlockPos> placeGroundPatch(
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        int xRadius,
        int zRadius
    ) {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos belowPos(origin.getX(), origin.getY(), origin.getZ());
        core::Direction inwards = CaveSurfaceHelper::getDirectionEnum(config.surface);
        core::Direction outwards = core::getOpposite(inwards);
        util::JavaHashSet<core::BlockPos> surface;

        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            bool isXEdge = dx == -xRadius || dx == xRadius;

            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                bool isZEdge = dz == -zRadius || dz == zRadius;
                bool isEdge = isXEdge || isZEdge;
                bool isCorner = isXEdge && isZEdge;
                bool isEdgeButNotCorner = isEdge && !isCorner;
                if (isCorner) {
                    continue;
                }
                if (isEdgeButNotCorner &&
                    (config.extraEdgeColumnChance == 0.0f || random.nextFloat() > config.extraEdgeColumnChance)) {
                    continue;
                }

                pos.setWithOffset(origin, dx, 0, dz);

                for (int offset = 0; offset < config.verticalRange && level->isStateAtPosition(pos, [](BlockState* state) {
                    return state && state->isAir();
                }); ++offset) {
                    pos.move(core::getStepX(inwards), core::getStepY(inwards), core::getStepZ(inwards));
                }

                for (int offset = 0; offset < config.verticalRange && level->isStateAtPosition(pos, [](BlockState* state) {
                    return state && !state->isAir();
                }); ++offset) {
                    pos.move(core::getStepX(outwards), core::getStepY(outwards), core::getStepZ(outwards));
                }

                belowPos.setWithOffset(pos, core::getStepX(inwards), core::getStepY(inwards), core::getStepZ(inwards));
                BlockState* belowState = level->getBlockState(belowPos);
                if (level->isEmptyBlock(pos) && belowState &&
                    belowState->isFaceSturdy(*level, belowPos, core::getOpposite(inwards))) {
                    int sampledDepth = config.depth->sample(random) +
                        ((config.extraBottomBlockChance > 0.0f && random.nextFloat() < config.extraBottomBlockChance) ? 1 : 0);
                    core::BlockPos groundPos(belowPos.getX(), belowPos.getY(), belowPos.getZ());
                    if (placeGround(level, config, random, belowPos, sampledDepth)) {
                        surface.add(groundPos);
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
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        WorldgenRandom& random,
        const util::JavaHashSet<core::BlockPos>& surface,
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
    virtual bool placeVegetation(
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        ChunkGenerator* generator,
        WorldgenRandom& random,
        const core::BlockPos& vegetationPos
    ) {
        core::Direction outwards = core::getOpposite(CaveSurfaceHelper::getDirectionEnum(config.surface));
        core::BlockPos placePos = vegetationPos.relative(outwards);

        if (config.vegetationFeature) {
            return config.vegetationFeature->place(level, generator, random, placePos);
        }
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
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        WorldgenRandom& random,
        core::BlockPos::MutableBlockPos& belowPos,
        int depth
    ) {
        core::Direction inwards = CaveSurfaceHelper::getDirectionEnum(config.surface);

        for (int i = 0; i < depth; ++i) {
            if (!config.groundState) {
                continue;
            }

            BlockState* stateToPlace = config.groundState->getState(random, belowPos);
            BlockState* belowState = level->getBlockState(belowPos);
            if (!stateToPlace || !belowState) {
                continue;
            }

            if (!stateToPlace->is(belowState->getBlock())) {
                if (!blockpredicates::matchesBlockTagName(belowState, config.replaceable)) {
                    return i != 0;
                }

                level->setBlock(belowPos, stateToPlace, 2);
                belowPos.move(core::getStepX(inwards), core::getStepY(inwards), core::getStepZ(inwards));
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

    BlockState* getState(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos) const {
        if (fallback) {
            return fallback->getState(random, pos);
        }
        return static_cast<BlockState*>(minecraft::world::level::block::Blocks::AIR->defaultBlockState());
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
        WorldGenLevel* worldLevel = context.level();
        WorldgenRandom& random = context.random();

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
                    placedAny |= placeColumn(config, worldLevel, random, top, bottom, mutablePos);
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
        WorldGenLevel* level,
        WorldgenRandom& random,
        int top,
        int bottom,
        core::BlockPos::MutableBlockPos& pos
    ) {
        bool placedAny = false;
        bool placedAbove = false;

        for (int y = top; y > bottom; --y) {
            pos.setY(y);

            // Check if target predicate matches
            // Reference: DiskFeature.java placeColumn() line 42
            if (config.target && config.target->test(*level, pos)) {
                if (config.stateProvider) {
                    // Get block state from provider (needs WorldGenLevel for rule-based selection)
                    BlockState* state = config.stateProvider->getState(level, random, pos);
                    // Place the block via WorldGenLevel (handles cross-chunk)
                    if (state) {
                        level->setBlock(pos, state, 2);
                    }

                    if (!placedAbove) {
                        // markAboveForPostProcessing via level
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
            blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE,
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
        WorldGenLevel* level = context.level();
        const BlockColumnConfiguration& config = context.config();
        WorldgenRandom& random = context.random();

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
        for (int y = 0; y < totalHeight; ++y) {
            if (config.allowedPlacement && !config.allowedPlacement->test(*level, nextPos)) {
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
                        if (state) {
                            level->setBlock(placePos, state, 2);
                        }
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
        WorldgenRandom& random = context.random();
        WorldGenLevel* level = context.level();

        // Reference: IceSpikeFeature.java lines 22-24
        // Move down until not empty
        while (true) {
            BlockState* block = level->getBlockState(origin);
            if (!block) break;
            BlockState* state = static_cast<BlockState*>(block);
            if (!state->isAir() || origin.getY() <= level->getMinY() + 2) break;
            origin = origin.below();
        }

        // Check for snow block
        BlockState* groundBlock = level->getBlockState(origin);
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
                        BlockState* targetBlock = level->getBlockState(placePos);
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
                            BlockState* belowBlock = level->getBlockState(belowPos);
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
                    BlockState* block = level->getBlockState(iceBlock);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        // Reference: GlowstoneFeature.java lines 21-22
        BlockState* originBlock = level->getBlockState(origin);
        if (originBlock) {
            BlockState* originState = static_cast<BlockState*>(originBlock);
            if (!originState->isAir()) {
                return false;
            }
        } else {
            return false;
        }

        // Reference: GlowstoneFeature.java lines 24-27
        BlockState* aboveBlock = level->getBlockState(origin.above());
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

            BlockState* block = level->getBlockState(placePos);
            if (!block) continue;
            BlockState* blockState = static_cast<BlockState*>(block);

            if (blockState->isAir()) {
                int neighbours = 0;

                // Check all 6 directions
                for (const auto& [dirX, dirY, dirZ] : DIRECTION_OFFSETS) {
                    core::BlockPos neighborPos = placePos.offset(dirX, dirY, dirZ);
                    BlockState* neighborBlock = level->getBlockState(neighborPos);
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
        WorldgenRandom& random = context.random();
        const core::BlockPos& origin = context.origin();
        WorldGenLevel* level = context.level();

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

            BlockState* block = level->getBlockState(pos);
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
        // Collect potential crystal placements
        std::vector<core::BlockPos> potentialCrystalPlacements;

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
                    // Reference: GeodeFeature.java lines 113-142
                    if (distSumShell >= outerCrust) {
                        if (shouldGenerateCrack && distSumCrack >= crackSize && distSumShell < innerAir) {
                            // Place air (crack)
                            level->setBlock(pointInside, world::level::block::Blocks::AIR->defaultBlockState(), 2);
                        } else if (distSumShell >= innerAir) {
                            // Place filling (air inside geode)
                            level->setBlock(pointInside, world::level::block::Blocks::AIR->defaultBlockState(), 2);
                        } else if (distSumShell >= innermostBlockLayer) {
                            // Place inner layer (budding amethyst area)
                            // CRITICAL: Must consume random for parity
                            bool useAlternate = random.nextFloat() < config.useAlternateLayer0Chance;
                            BlockState* innerState = useAlternate ?
                                world::level::block::Blocks::BUDDING_AMETHYST->defaultBlockState() :
                                world::level::block::Blocks::AMETHYST_BLOCK->defaultBlockState();
                            level->setBlock(pointInside, innerState, 2);

                            // Reference: GeodeFeature.java lines 134-136
                            // Collect potential crystal placement positions
                            if ((!config.placementsRequireLayer0Alternate || useAlternate) &&
                                random.nextFloat() < config.usePotentialPlacementsChance) {
                                potentialCrystalPlacements.push_back(pointInside);
                            }
                        } else if (distSumShell >= innerCrust) {
                            // Place middle layer (calcite)
                            level->setBlock(pointInside, world::level::block::Blocks::CALCITE->defaultBlockState(), 2);
                        } else {
                            // Place outer layer (smooth basalt)
                            level->setBlock(pointInside, world::level::block::Blocks::SMOOTH_BASALT->defaultBlockState(), 2);
                        }
                    }
                }
            }
        }

        // Reference: GeodeFeature.java lines 145-166
        // Place amethyst buds/clusters on inner surfaces
        const std::vector<BlockState*>& innerPlacements = config.geodeBlockSettings.innerPlacements;
        if (!innerPlacements.empty()) {
            static const std::vector<core::Direction> DIRECTIONS = {
                core::Direction::UP, core::Direction::DOWN,
                core::Direction::NORTH, core::Direction::SOUTH,
                core::Direction::EAST, core::Direction::WEST
            };

            for (const core::BlockPos& crystalPos : potentialCrystalPlacements) {
                // Pick random inner placement (amethyst bud type)
                int budIndex = random.nextInt(static_cast<int>(innerPlacements.size()));
                BlockState* budState = innerPlacements[budIndex];

                // Try each direction to place the bud
                for (core::Direction dir : DIRECTIONS) {
                    core::BlockPos placePos = crystalPos.relative(dir);
                    BlockState* placeState = level->getBlockState(placePos);

                    // Can place if the target position is air or water
                    // Reference: BuddingAmethystBlock.canClusterGrowAtState()
                    if (placeState && (placeState->isAir() ||
                        placeState->getIdentifier() == "minecraft:water")) {
                        level->setBlock(placePos, budState, 2);
                        break;
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const HugeFungusConfiguration& config = context.config();

        // Check base block
        BlockState* belowBlock = level->getBlockState(origin.below());
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();

        // Reference: BlueIceFeature.java lines 21-26
        // Check position is in packed ice
        BlockState* block = level->getBlockState(origin);
        if (!block) return false;
        BlockState* state = static_cast<BlockState*>(block);
        if (state->getIdentifier() != "minecraft:packed_ice") {
            return false;
        }

        // Check above is air
        BlockState* aboveBlock = level->getBlockState(origin.above());
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

            BlockState* targetBlock = level->getBlockState(placePos);
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
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();

        // Reference: ChorusPlantFeature.java lines 17-24
        // Check if origin is air and below is end stone
        BlockState* originBlock = level->getBlockState(origin);
        if (!originBlock) return false;
        BlockState* originState = static_cast<BlockState*>(originBlock);
        if (!originState->isAir()) {
            return false;
        }

        BlockState* belowBlock = level->getBlockState(origin.below());
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        const NetherForestVegetationConfiguration& config = context.config();
        WorldgenRandom& random = context.random();

        BlockState* belowBlock = level->getBlockState(origin.below());
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
                BlockState* targetBlock = level->getBlockState(finalPos);
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
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();
        const ProbabilityFeatureConfiguration& config = context.config();

        BlockState* originBlock = level->getBlockState(origin);
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
            BlockState* block = level->getBlockState(bambooPos);
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
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();
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
        WorldgenRandom& random = context.random();
        WorldGenLevel* level = context.level();
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
    /**
     * Place seagrass
     * Reference: SeagrassFeature.java place() lines 19-49
     *
     * CRITICAL RANDOM ORDER:
     * 1. nextInt(8) x4 for X, Z offsets
     * 2. nextDouble() for tall/short decision
     */
    bool place(FeaturePlaceContext<ProbabilityFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const ProbabilityFeatureConfiguration& config = context.config();

        // Reference: SeagrassFeature.java lines 25-26
        // CRITICAL: Must consume random in this exact order for parity
        int dx = random.nextInt(8) - random.nextInt(8);
        int dz = random.nextInt(8) - random.nextInt(8);

        // Reference: SeagrassFeature.java line 27
        int y = level->getHeight(Heightmap::Types::OCEAN_FLOOR, origin.getX() + dx, origin.getZ() + dz);
        core::BlockPos grassPos(origin.getX() + dx, y, origin.getZ() + dz);

        // Reference: SeagrassFeature.java lines 29-46
        BlockState* blockAtPos = level->getBlockState(grassPos);
        if (blockAtPos && blockAtPos->getIdentifier() == "minecraft:water") {
            // Reference: SeagrassFeature.java line 30
            // CRITICAL: Must consume nextDouble for parity
            bool isTall = random.nextDouble() < static_cast<double>(config.probability);

            // Get block states
            BlockState* seagrassState = world::level::block::Blocks::SEAGRASS ?
                world::level::block::Blocks::SEAGRASS->defaultBlockState() : nullptr;
            BlockState* tallSeagrassState = world::level::block::Blocks::TALL_SEAGRASS ?
                world::level::block::Blocks::TALL_SEAGRASS->defaultBlockState() : nullptr;

            if (isTall && tallSeagrassState) {
                // Place tall seagrass (double plant)
                core::BlockPos above = grassPos.above();
                BlockState* aboveBlock = level->getBlockState(above);
                if (aboveBlock && aboveBlock->getIdentifier() == "minecraft:water") {
                    // Place lower half
                    level->setBlock(grassPos, tallSeagrassState, 2);
                    // Place upper half (TODO: would need HALF property set to UPPER)
                    level->setBlock(above, tallSeagrassState, 2);
                    return true;
                }
            } else if (seagrassState) {
                // Place single seagrass
                level->setBlock(grassPos, seagrassState, 2);
                return true;
            }
        }

        return false;
    }
};

//=============================================================================
// KelpFeature
// Reference: KelpFeature.java
//=============================================================================

class KelpFeature : public Feature<NoneFeatureConfiguration> {
public:
    /**
     * Place kelp column
     * Reference: KelpFeature.java place() lines 18-52
     *
     * CRITICAL RANDOM ORDER:
     * 1. nextInt(10) for height (1 + result)
     * 2. nextInt(4) + 20 for AGE property (at top or when breaking)
     */
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        int placed = 0;

        // Reference: KelpFeature.java line 23
        int y = level->getHeight(Heightmap::Types::OCEAN_FLOOR, origin.getX(), origin.getZ());
        core::BlockPos kelpPos(origin.getX(), y, origin.getZ());

        // Reference: KelpFeature.java line 25
        BlockState* blockAtPos = level->getBlockState(kelpPos);
        if (blockAtPos && blockAtPos->getIdentifier() == "minecraft:water") {
            // Get block states
            BlockState* kelpTopState = world::level::block::Blocks::KELP ?
                world::level::block::Blocks::KELP->defaultBlockState() : nullptr;
            BlockState* kelpPlantState = world::level::block::Blocks::KELP_PLANT ?
                world::level::block::Blocks::KELP_PLANT->defaultBlockState() : nullptr;

            // Reference: KelpFeature.java line 28
            // CRITICAL: Must consume nextInt(10) for parity
            int height = 1 + random.nextInt(10);

            // Reference: KelpFeature.java lines 30-48
            for (int h = 0; h <= height; ++h) {
                BlockState* currentBlock = level->getBlockState(kelpPos);
                BlockState* aboveBlock = level->getBlockState(kelpPos.above());

                bool currentIsWater = currentBlock && currentBlock->getIdentifier() == "minecraft:water";
                bool aboveIsWater = aboveBlock && aboveBlock->getIdentifier() == "minecraft:water";

                if (currentIsWater && aboveIsWater && kelpPlantState) {
                    if (h == height) {
                        // Reference: KelpFeature.java line 33
                        // CRITICAL: Must consume nextInt(4) for AGE property
                        int age = random.nextInt(4) + 20;
                        (void)age; // TODO: Set AGE property on kelpTopState
                        level->setBlock(kelpPos, kelpTopState, 2);
                        ++placed;
                    } else {
                        // Place kelp plant (body segment)
                        level->setBlock(kelpPos, kelpPlantState, 2);
                    }
                } else if (h > 0) {
                    // Reference: KelpFeature.java lines 38-44
                    // Early termination - place top block below
                    core::BlockPos below = kelpPos.below();
                    BlockState* belowBlock = level->getBlockState(below);
                    BlockState* belowBelowBlock = level->getBlockState(below.below());

                    bool canPlace = belowBlock && belowBlock->getIdentifier() != "minecraft:kelp";
                    bool notKelpBelow = belowBelowBlock && belowBelowBlock->getIdentifier() != "minecraft:kelp";

                    if (canPlace && notKelpBelow && kelpTopState) {
                        // CRITICAL: Must consume nextInt(4) for AGE property
                        int age = random.nextInt(4) + 20;
                        (void)age; // TODO: Set AGE property on kelpTopState
                        level->setBlock(below, kelpTopState, 2);
                        ++placed;
                    }
                    break;
                }

                kelpPos = kelpPos.above();
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        if (!level->isEmptyBlock(origin)) {
            return false;
        }

        if (!dynamic_cast<world::level::block::VineBlock*>(world::level::block::Blocks::VINE)) {
            return false;
        }

        for (core::Direction direction : {
                 core::Direction::DOWN,
                 core::Direction::UP,
                 core::Direction::NORTH,
                 core::Direction::SOUTH,
                 core::Direction::WEST,
                 core::Direction::EAST
             }) {
            if (direction == core::Direction::DOWN) {
                continue;
            }

            if (world::level::block::VineBlock::isAcceptableNeighbour(*level, origin.relative(direction), direction)) {
                if (auto* property = world::level::block::VineBlock::getPropertyForFace(direction)) {
                    level->setBlock(
                        origin,
                        world::level::block::Blocks::VINE->defaultBlockState()->setValue(*property, true),
                        2
                    );
                    return true;
                }
            }
        }

        return false;
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
    // Uses WorldGenLevel for cross-chunk placement support
    void placeTrunk(
        WorldGenLevel* level,
        WorldgenRandom& random,
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
    // Uses WorldGenLevel.setBlock() which handles cross-chunk writes
    void placeMushroomBlock(
        WorldGenLevel* level,
        const core::BlockPos& pos,
        BlockState* newState
    ) {
        // WorldGenLevel.ensureCanWrite() checks if position is within write radius
        if (!level->ensureCanWrite(pos)) {
            return;
        }

        BlockState* currentBlock = level->getBlockState(pos);
        if (currentBlock) {
            // Check block properties via BlockState
            // Check if block is air or replaceable by mushroom
            // Reference: BlockTags.REPLACEABLE_BY_MUSHROOMS
            const std::string& name = currentBlock->getIdentifier();
            if (currentBlock->isAir() ||
                currentBlock->isLeaves() ||
                name == "minecraft:brown_mushroom_block" ||
                name == "minecraft:red_mushroom_block" ||
                name == "minecraft:mushroom_stem" ||
                name == "minecraft:short_grass" ||
                name == "minecraft:fern" ||
                name == "minecraft:dead_bush" ||
                name == "minecraft:bush" ||
                name == "minecraft:vine" ||
                name == "minecraft:tall_grass" ||
                name == "minecraft:large_fern" ||
                name == "minecraft:snow") {
                // Use WorldGenLevel.setBlock() for cross-chunk support
                level->setBlock(pos, newState, 3);  // flags = 3 like Java
            }
        } else {
            // No current block means we can place
            level->setBlock(pos, newState, 3);
        }
    }

    // Reference: AbstractHugeMushroomFeature.java getTreeHeight() lines 34-41
    int getTreeHeight(WorldgenRandom& random) {
        int treeHeight = random.nextInt(3) + 4;
        if (random.nextInt(12) == 0) {
            treeHeight *= 2;
        }
        return treeHeight;
    }

    // Reference: AbstractHugeMushroomFeature.java isValidPosition() lines 43-68
    bool isValidPosition(
        WorldGenLevel* level,
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
        BlockState* belowBlock = level->getBlockState(below);
        if (belowBlock) {
            // Check block properties via BlockState
            // Original: !belowState.isDirt() && !belowState.is("minecraft:mycelium")
            const std::string& name = belowBlock->getIdentifier();
            bool isDirt = name == "minecraft:dirt" ||
                          name == "minecraft:grass_block" ||
                          name == "minecraft:coarse_dirt" ||
                          name == "minecraft:podzol";
            if (!isDirt && name != "minecraft:mycelium") {
                return false;
            }
        }

        for (int dy = 0; dy <= treeHeight; ++dy) {
            int radius = getTreeRadiusForHeight(-1, -1, config.foliageRadius, dy);
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    blockPos.set(origin.getX() + dx, origin.getY() + dy, origin.getZ() + dz);
                    BlockState* block = level->getBlockState(blockPos);
                    if (block) {
                        // Check block properties via BlockState
                        if (!block->isAir() && !block->isLeaves()) {
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
        WorldGenLevel* level,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        int treeHeight,
        core::BlockPos::MutableBlockPos& blockPos,
        const HugeMushroomFeatureConfiguration& config
    ) = 0;

public:
    // Reference: AbstractHugeMushroomFeature.java place() lines 70-84
    bool place(FeaturePlaceContext<HugeMushroomFeatureConfiguration>& context) override {
        // Use WorldGenLevel for cross-chunk support (like Java)
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        BlockState* state
    ) {
        core::BlockPos above = pos.above();
        BlockState* targetBlock = level->getBlockState(pos);
        BlockState* aboveBlock = level->getBlockState(above);

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
        WorldGenLevel* level,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        BlockState* state
    ) = 0;

public:
    // Reference: CoralFeature.java place() lines 25-31
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        // Pick a random coral block type
        BlockState* coralState = static_cast<BlockState*>(minecraft::world::level::block::Blocks::getDefaultState("minecraft:brain_coral_block")); // Simplified
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level,
        WorldgenRandom& random,
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

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
        WorldGenLevel* level = context.level();
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
        WorldGenLevel* level = context.level();
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
    static void createEndPlatform(WorldGenLevel* level, const core::BlockPos& origin) {
        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        for (int dz = -2; dz <= 2; ++dz) {
            for (int dx = -2; dx <= 2; ++dx) {
                for (int dy = -1; dy < 3; ++dy) {
                    pos.set(origin.getX() + dx, origin.getY() + dy, origin.getZ() + dz);
                    // dy == -1: place OBSIDIAN
                    // dy >= 0: place AIR
                    // TODO: Actually place blocks using level->setBlock()
                }
            }
        }
    }
};

//=============================================================================
// Column - Cave column scanning utility
// Reference: net/minecraft/world/level/levelgen/Column.java
//=============================================================================

struct ColumnResult {
    std::optional<int> floor;
    std::optional<int> ceiling;

    std::optional<int> getFloor() const { return floor; }
    std::optional<int> getCeiling() const { return ceiling; }
    std::optional<int> getHeight() const {
        if (floor.has_value() && ceiling.has_value())
            return *ceiling - *floor - 1;
        return std::nullopt;
    }

    ColumnResult withFloor(std::optional<int> newFloor) const {
        return ColumnResult{newFloor, ceiling};
    }

    bool isRange() const { return floor.has_value() && ceiling.has_value(); }
    int height() const { return *ceiling - *floor - 1; }
};

namespace ColumnScan {

inline std::optional<int> scanDirection(
    WorldGenLevel* level,
    int searchRange,
    std::function<bool(BlockState*)> insideColumn,
    std::function<bool(BlockState*)> validEdge,
    core::BlockPos::MutableBlockPos& pos,
    int startY,
    core::Direction direction
) {
    pos.setY(startY);
    for (int i = 1; i < searchRange; ++i) {
        BlockState* state = level->getBlockState(pos);
        if (!state || !insideColumn(state)) break;
        pos.move(direction);
    }
    BlockState* edgeState = level->getBlockState(pos);
    if (edgeState && validEdge(edgeState)) {
        return pos.getY();
    }
    return std::nullopt;
}

inline std::optional<ColumnResult> scan(
    WorldGenLevel* level,
    const core::BlockPos& pos,
    int searchRange,
    std::function<bool(BlockState*)> insideColumn,
    std::function<bool(BlockState*)> validEdge
) {
    BlockState* originState = level->getBlockState(pos);
    if (!originState || !insideColumn(originState)) {
        return std::nullopt;
    }

    core::BlockPos::MutableBlockPos mutablePos(pos.getX(), pos.getY(), pos.getZ());
    int nearestEmptyY = pos.getY();

    auto ceiling = scanDirection(level, searchRange, insideColumn, validEdge, mutablePos, nearestEmptyY, core::Direction::UP);
    auto floor = scanDirection(level, searchRange, insideColumn, validEdge, mutablePos, nearestEmptyY, core::Direction::DOWN);

    return ColumnResult{floor, ceiling};
}

} // namespace ColumnScan

//=============================================================================
// DripstoneUtils - Helper functions for dripstone features
// Reference: net/minecraft/world/level/levelgen/feature/DripstoneUtils.java
//=============================================================================

namespace DripstoneUtils {

inline bool isEmptyOrWater(BlockState* state) {
    return state->isAir() || state->getIdentifier() == "minecraft:water";
}

inline bool isNeitherEmptyNorWater(BlockState* state) {
    return !state->isAir() && state->getIdentifier() != "minecraft:water";
}

inline bool isEmptyOrWaterOrLava(BlockState* state) {
    return state->isAir() || state->getIdentifier() == "minecraft:water" || state->getIdentifier() == "minecraft:lava";
}

inline bool isDripstoneReplaceable(BlockState* state) {
    // BlockTags.DRIPSTONE_REPLACEABLE = BASE_STONE_OVERWORLD
    const std::string& id = state->getIdentifier();
    return id == "minecraft:stone" || id == "minecraft:granite" || id == "minecraft:diorite" ||
           id == "minecraft:andesite" || id == "minecraft:tuff" || id == "minecraft:deepslate";
}

inline bool isBaseStoneOverworld(BlockState* state) {
    return isDripstoneReplaceable(state);
}

inline bool isDripstoneBase(BlockState* state) {
    return state->getIdentifier() == "minecraft:dripstone_block" || isDripstoneReplaceable(state);
}

inline bool isDripstoneBaseOrLava(BlockState* state) {
    return isDripstoneBase(state) || state->getIdentifier() == "minecraft:lava";
}

inline bool isEmptyOrWaterLevel(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    return state && isEmptyOrWater(state);
}

inline bool isEmptyOrWaterOrLavaLevel(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    return state && isEmptyOrWaterOrLava(state);
}

inline bool placeDripstoneBlockIfPossible(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    if (state && isDripstoneReplaceable(state)) {
        level->setBlock(pos, world::level::block::Blocks::getDefaultState("minecraft:dripstone_block"), 2);
        return true;
    }
    return false;
}

inline void growPointedDripstone(
    WorldGenLevel* level,
    const core::BlockPos& startPos,
    core::Direction tipDirection,
    int height,
    bool mergedTip
) {
    // Check base block
    core::BlockPos baseCheckPos = startPos.relative(core::getOpposite(tipDirection));
    BlockState* baseState = level->getBlockState(baseCheckPos);
    if (!baseState || !isDripstoneBase(baseState)) return;

    BlockState* dripstoneState = world::level::block::Blocks::getDefaultState("minecraft:pointed_dripstone");
    if (!dripstoneState) return;

    // Build column from base to tip
    // Reference: DripstoneUtils.java buildBaseToTipColumn() + growPointedDripstone()
    core::BlockPos::MutableBlockPos pos(startPos.getX(), startPos.getY(), startPos.getZ());

    // Generate thickness values: BASE, MIDDLE..., FRUSTUM, TIP/TIP_MERGE
    // For height >= 3: BASE + (height-3)*MIDDLE + FRUSTUM + TIP
    // For height == 2: FRUSTUM + TIP
    // For height == 1: TIP
    for (int i = 0; i < height; ++i) {
        // Place pointed_dripstone block
        // Check if position should be waterlogged
        BlockState* existingState = level->getBlockState(pos);
        bool isWater = existingState && existingState->getIdentifier() == "minecraft:water";

        level->setBlock(pos, dripstoneState, 2);
        pos.move(tipDirection);
    }
}

inline double getDripstoneHeight(double xzDistanceFromCenter, double dripstoneRadius, double scale, double bluntness) {
    if (xzDistanceFromCenter < bluntness) {
        xzDistanceFromCenter = bluntness;
    }
    double r = xzDistanceFromCenter / dripstoneRadius * 0.384;
    double part1 = 0.75 * std::pow(r, 1.3333333333333333);
    double part2 = std::pow(r, 0.6666666666666666);
    double part3 = 0.3333333333333333 * std::log(r);
    double heightRelativeToMaxRadius = scale * (part1 - part2 - part3);
    heightRelativeToMaxRadius = std::max(heightRelativeToMaxRadius, 0.0);
    return heightRelativeToMaxRadius / 0.384 * dripstoneRadius;
}

inline bool isCircleMostlyEmbeddedInStone(WorldGenLevel* level, const core::BlockPos& center, int xzRadius) {
    if (isEmptyOrWaterOrLavaLevel(level, center)) return false;

    float angleIncrement = 6.0f / static_cast<float>(xzRadius);
    for (float angle = 0.0f; angle < 6.2831853f; angle += angleIncrement) {
        int dx = static_cast<int>(Mth::cos(static_cast<double>(angle)) * static_cast<float>(xzRadius));
        int dz = static_cast<int>(Mth::sin(static_cast<double>(angle)) * static_cast<float>(xzRadius));
        if (isEmptyOrWaterOrLavaLevel(level, center.offset(dx, 0, dz))) {
            return false;
        }
    }
    return true;
}

} // namespace DripstoneUtils

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
        WorldGenLevel* level = context.level();
        core::BlockPos pos = context.origin();
        WorldgenRandom& random = context.random();
        const PointedDripstoneConfiguration& config = context.config();

        auto tipDirection = getTipDirection(level, pos, random);
        if (!tipDirection.has_value()) {
            return false;
        }

        core::BlockPos rootPos = pos.relative(core::getOpposite(*tipDirection));
        createPatchOfDripstoneBlocks(level, random, rootPos, config);

        int height = (random.nextFloat() < config.chanceOfTallerDripstone &&
                      DripstoneUtils::isEmptyOrWater(level->getBlockState(pos.relative(*tipDirection))))
                     ? 2 : 1;

        DripstoneUtils::growPointedDripstone(level, pos, *tipDirection, height, false);
        return true;
    }

private:
    // Reference: PointedDripstoneFeature.java getTipDirection() lines 33-43
    static std::optional<core::Direction> getTipDirection(
        WorldGenLevel* level,
        const core::BlockPos& pos,
        WorldgenRandom& random
    ) {
        BlockState* aboveBlock = level->getBlockState(pos.above());
        BlockState* belowBlock = level->getBlockState(pos.below());

        bool canPlaceAbove = aboveBlock && DripstoneUtils::isDripstoneBase(aboveBlock);
        bool canPlaceBelow = belowBlock && DripstoneUtils::isDripstoneBase(belowBlock);

        if (canPlaceAbove && canPlaceBelow) {
            return random.nextBoolean() ? core::Direction::DOWN : core::Direction::UP;
        } else if (canPlaceAbove) {
            return core::Direction::DOWN;
        } else if (canPlaceBelow) {
            return core::Direction::UP;
        }
        return std::nullopt;
    }

    // Reference: PointedDripstoneFeature.java createPatchOfDripstoneBlocks() lines 45-62
    static void createPatchOfDripstoneBlocks(
        WorldGenLevel* level,
        WorldgenRandom& random,
        const core::BlockPos& pos,
        const PointedDripstoneConfiguration& config
    ) {
        DripstoneUtils::placeDripstoneBlockIfPossible(level, pos);

        // Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST
        core::Direction horizontals[] = {
            core::Direction::NORTH, core::Direction::EAST,
            core::Direction::SOUTH, core::Direction::WEST
        };
        for (auto direction : horizontals) {
            if (!(random.nextFloat() > config.chanceOfDirectionalSpread)) {
                core::BlockPos pos1 = pos.relative(direction);
                DripstoneUtils::placeDripstoneBlockIfPossible(level, pos1);
                if (!(random.nextFloat() > config.chanceOfSpreadRadius2)) {
                    core::BlockPos pos2 = pos1.relative(core::fromIndex(random.nextInt(6)));
                    DripstoneUtils::placeDripstoneBlockIfPossible(level, pos2);
                    if (!(random.nextFloat() > config.chanceOfSpreadRadius3)) {
                        core::BlockPos pos3 = pos2.relative(core::fromIndex(random.nextInt(6)));
                        DripstoneUtils::placeDripstoneBlockIfPossible(level, pos3);
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
    std::shared_ptr<levelgen::carver::FloatProvider> stalactiteBluntness;
    std::shared_ptr<levelgen::carver::FloatProvider> stalagmiteBluntness;
    std::shared_ptr<levelgen::carver::FloatProvider> heightScale;
    std::shared_ptr<levelgen::carver::FloatProvider> windSpeed;
    int minRadiusForWind = 1;
    float minBluntnessForWind = 0.0f;

    LargeDripstoneConfiguration() = default;
};

//=============================================================================
// LargeDripstoneFeature
// Reference: LargeDripstoneFeature.java
//=============================================================================

class LargeDripstoneFeature : public Feature<LargeDripstoneConfiguration> {
    // Inner class for wind displacement
    struct WindOffsetter {
        int originY = 0;
        bool hasWind = false;
        double windX = 0, windZ = 0;

        WindOffsetter() : hasWind(false) {}
        WindOffsetter(int originY, WorldgenRandom& random, levelgen::carver::FloatProvider* windSpeedRange)
            : originY(originY), hasWind(true) {
            float speed = windSpeedRange->sample(random);
            float direction = Mth::randomBetween(random, 0.0f, static_cast<float>(M_PI));
            windX = static_cast<double>(Mth::cos(static_cast<double>(direction)) * speed);
            windZ = static_cast<double>(Mth::sin(static_cast<double>(direction)) * speed);
        }

        core::BlockPos offset(const core::BlockPos& pos) const {
            if (!hasWind) return pos;
            int dy = originY - pos.getY();
            return pos.offset(Mth::floor(windX * dy), 0, Mth::floor(windZ * dy));
        }
    };

    // Inner class for large dripstone column shape
    struct LargeDripstone {
        core::BlockPos root;
        bool pointingUp;
        int radius;
        double bluntness;
        double scale;

        int getHeight() const { return getHeightAtRadius(0.0f); }
        int getHeightAtRadius(float checkRadius) const {
            return static_cast<int>(DripstoneUtils::getDripstoneHeight(
                static_cast<double>(checkRadius), static_cast<double>(radius), scale, bluntness));
        }

        bool moveBackUntilBaseIsInsideStone(WorldGenLevel* level, const WindOffsetter& wind) {
            while (radius > 1) {
                core::BlockPos::MutableBlockPos newRoot(root.getX(), root.getY(), root.getZ());
                int maxTries = std::min(10, getHeight());
                for (int i = 0; i < maxTries; ++i) {
                    BlockState* state = level->getBlockState(newRoot);
                    if (state && state->getIdentifier() == "minecraft:lava") return false;
                    if (DripstoneUtils::isCircleMostlyEmbeddedInStone(level, wind.offset(newRoot), radius)) {
                        root = core::BlockPos(newRoot.getX(), newRoot.getY(), newRoot.getZ());
                        return true;
                    }
                    newRoot.move(pointingUp ? core::Direction::DOWN : core::Direction::UP);
                }
                radius /= 2;
            }
            return false;
        }

        void placeBlocks(WorldGenLevel* level, WorldgenRandom& random, const WindOffsetter& wind) {
            BlockState* dripstoneBlock = world::level::block::Blocks::getDefaultState("minecraft:dripstone_block");
            if (!dripstoneBlock) return;

            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    float currentRadius = Mth::sqrt(static_cast<float>(dx * dx + dz * dz));
                    if (currentRadius > static_cast<float>(radius)) continue;

                    int height = getHeightAtRadius(currentRadius);
                    if (height <= 0) continue;

                    if (static_cast<double>(random.nextFloat()) < 0.2) {
                        height = static_cast<int>(static_cast<float>(height) * Mth::randomBetween(random, 0.8f, 1.0f));
                    }

                    core::BlockPos::MutableBlockPos pos(root.getX() + dx, root.getY(), root.getZ() + dz);
                    bool hasBeenOutOfStone = false;
                    int maxY = pointingUp ? level->getHeight(levelgen::Heightmap::Types::WORLD_SURFACE_WG, pos.getX(), pos.getZ()) : INT_MAX;

                    for (int i = 0; i < height && pos.getY() < maxY; ++i) {
                        core::BlockPos windPos = wind.offset(pos);
                        if (DripstoneUtils::isEmptyOrWaterOrLavaLevel(level, windPos)) {
                            hasBeenOutOfStone = true;
                            level->setBlock(windPos, dripstoneBlock, 2);
                        } else if (hasBeenOutOfStone) {
                            BlockState* state = level->getBlockState(windPos);
                            if (state && DripstoneUtils::isBaseStoneOverworld(state)) break;
                        }
                        pos.move(pointingUp ? core::Direction::UP : core::Direction::DOWN);
                    }
                }
            }
        }

        bool isSuitableForWind(const LargeDripstoneConfiguration& config) const {
            return radius >= config.minRadiusForWind && bluntness >= static_cast<double>(config.minBluntnessForWind);
        }
    };

public:
    // Reference: LargeDripstoneFeature.java place() lines 26-71
    // TODO: Implementation exists but produces non-matching output. Disabled pending
    // exact RNG parity debugging.
    bool place(FeaturePlaceContext<LargeDripstoneConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const LargeDripstoneConfiguration& config = context.config();

        if (!DripstoneUtils::isEmptyOrWaterLevel(level, origin)) return false;

        auto column = ColumnScan::scan(level, origin, config.floorToCeilingSearchRange,
            DripstoneUtils::isEmptyOrWater, DripstoneUtils::isDripstoneBaseOrLava);
        if (!column.has_value() || !column->isRange()) return false;
        if (column->height() < 4) return false;

        int maxColRadius = static_cast<int>(static_cast<float>(column->height()) * config.maxColumnRadiusToCaveHeightRatio);
        int minRadius = config.columnRadius ? config.columnRadius->getMinValue() : 3;
        int maxRadius = config.columnRadius ? config.columnRadius->getMaxValue() : 19;
        maxColRadius = std::clamp(maxColRadius, minRadius, maxRadius);
        int radius = Mth::randomBetweenInclusive(random, minRadius, maxColRadius);

        // Reference: LargeDripstoneFeature.java makeDripstone() - samples bluntness then heightScale per dripstone
        // Java order: stalactite(bluntness, heightScale), stalagmite(bluntness, heightScale)
        float stalactiteBluntVal = config.stalactiteBluntness ? config.stalactiteBluntness->sample(random) : 0.7f;
        float stalactiteHeightScaleVal = config.heightScale ? config.heightScale->sample(random) : 1.0f;
        float stalagmiteBluntVal = config.stalagmiteBluntness ? config.stalagmiteBluntness->sample(random) : 0.7f;
        float stalagmiteHeightScaleVal = config.heightScale ? config.heightScale->sample(random) : 1.0f;

        LargeDripstone stalactite{
            origin.atY(*column->ceiling - 1), false, radius,
            static_cast<double>(stalactiteBluntVal), static_cast<double>(stalactiteHeightScaleVal)
        };
        LargeDripstone stalagmite{
            origin.atY(*column->floor + 1), true, radius,
            static_cast<double>(stalagmiteBluntVal), static_cast<double>(stalagmiteHeightScaleVal)
        };

        WindOffsetter wind;
        if (stalactite.isSuitableForWind(config) && stalagmite.isSuitableForWind(config) && config.windSpeed) {
            wind = WindOffsetter(origin.getY(), random, config.windSpeed.get());
        }

        bool stalactiteOk = stalactite.moveBackUntilBaseIsInsideStone(level, wind);
        bool stalagmiteOk = stalagmite.moveBackUntilBaseIsInsideStone(level, wind);

        if (stalactiteOk) stalactite.placeBlocks(level, random, wind);
        if (stalagmiteOk) stalagmite.placeBlocks(level, random, wind);

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
    std::shared_ptr<levelgen::carver::FloatProvider> wetness;
    std::shared_ptr<levelgen::carver::FloatProvider> density;
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
    // TODO: Implementation exists but produces non-matching output. Disabled pending
    // exact RNG parity debugging.
    bool place(FeaturePlaceContext<DripstoneClusterConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const DripstoneClusterConfiguration& config = context.config();

        if (!DripstoneUtils::isEmptyOrWaterLevel(level, origin)) return false;

        int clusterHeight = config.height ? config.height->sample(random) : 5;
        float wetness = config.wetness ? config.wetness->sample(random) : 0.5f;
        float density = config.density ? config.density->sample(random) : 0.5f;
        int xRadius = config.radius ? config.radius->sample(random) : 3;
        int zRadius = config.radius ? config.radius->sample(random) : 3;

        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                double chance = getChanceOfStalagmiteOrStalactite(xRadius, zRadius, dx, dz, config);
                core::BlockPos pos = origin.offset(dx, 0, dz);
                placeColumn(level, random, pos, dx, dz, wetness, chance, clusterHeight, density, config);
            }
        }

        return true;
    }

private:
    // Reference: DripstoneClusterFeature.java getChanceOfStalagmiteOrStalactite()
    // CRITICAL: Java uses float clampedMap, NOT double. The result is cast to double
    // for comparison with random.nextDouble().
    double getChanceOfStalagmiteOrStalactite(int xRadius, int zRadius, int dx, int dz,
        const DripstoneClusterConfiguration& config) {
        int xDistFromEdge = xRadius - std::abs(dx);
        int zDistFromEdge = zRadius - std::abs(dz);
        int distFromEdge = std::min(xDistFromEdge, zDistFromEdge);
        return static_cast<double>(Mth::clampedMap(
            static_cast<float>(distFromEdge), 0.0f,
            static_cast<float>(config.maxDistanceFromEdgeAffectingChanceOfDripstoneColumn),
            config.chanceOfDripstoneColumnAtMaxDistanceFromCenter, 1.0f));
    }

    void placeColumn(WorldGenLevel* level, WorldgenRandom& random,
        const core::BlockPos& pos, int dx, int dz, float chanceOfWater,
        double chanceOfStalagmiteOrStalactite, int clusterHeight, float density,
        const DripstoneClusterConfiguration& config
    ) {
        auto baseColumn = ColumnScan::scan(level, pos, config.floorToCeilingSearchRange,
            DripstoneUtils::isEmptyOrWater, DripstoneUtils::isNeitherEmptyNorWater);
        if (!baseColumn.has_value()) return;

        auto ceiling = baseColumn->getCeiling();
        auto baseFloor = baseColumn->getFloor();
        if (!ceiling.has_value() && !baseFloor.has_value()) return;

        bool wantPool = random.nextFloat() < chanceOfWater;
        ColumnResult column = *baseColumn;
        if (wantPool && baseFloor.has_value() && canPlacePool(level, pos.atY(*baseFloor))) {
            int baseFloorY = *baseFloor;
            column = baseColumn->withFloor(baseFloorY - 1);
            level->setBlock(pos.atY(baseFloorY),
                world::level::block::Blocks::getDefaultState("minecraft:water"), 2);
        }

        auto floor = column.getFloor();
        bool wantStalactite = random.nextDouble() < chanceOfStalagmiteOrStalactite;
        int stalactiteHeight = 0;
        if (ceiling.has_value() && wantStalactite && !isLava(level, pos.atY(*ceiling))) {
            int ceilingThickness = config.dripstoneBlockLayerThickness
                ? config.dripstoneBlockLayerThickness->sample(random) : 2;
            replaceBlocksWithDripstoneBlocks(level, pos.atY(*ceiling), ceilingThickness, core::Direction::UP);
            int maxHeightForCol = floor.has_value()
                ? std::min(clusterHeight, *ceiling - *floor)
                : clusterHeight;
            stalactiteHeight = getDripstoneHeight(random, dx, dz, density, maxHeightForCol, config);
        }

        bool wantStalagmite = random.nextDouble() < chanceOfStalagmiteOrStalactite;
        int stalagmiteHeight = 0;
        if (floor.has_value() && wantStalagmite && !isLava(level, pos.atY(*floor))) {
            int floorThickness = config.dripstoneBlockLayerThickness
                ? config.dripstoneBlockLayerThickness->sample(random) : 2;
            replaceBlocksWithDripstoneBlocks(level, pos.atY(*floor), floorThickness, core::Direction::DOWN);
            if (ceiling.has_value()) {
                stalagmiteHeight = std::max(0, stalactiteHeight +
                    Mth::randomBetweenInclusive(random, -config.maxStalagmiteStalactiteHeightDiff,
                                                 config.maxStalagmiteStalactiteHeightDiff));
            } else {
                stalagmiteHeight = getDripstoneHeight(random, dx, dz, density, clusterHeight, config);
            }
        }

        int actualStalactiteHeight, actualStalagmiteHeight;
        if (ceiling.has_value() && floor.has_value() &&
            *ceiling - stalactiteHeight <= *floor + stalagmiteHeight) {
            int floorY = *floor;
            int ceilingY = *ceiling;
            int lowestBottom = std::max(ceilingY - stalactiteHeight, floorY + 1);
            int highestTop = std::min(floorY + stalagmiteHeight, ceilingY - 1);
            int actualBottom = Mth::randomBetweenInclusive(random, lowestBottom, highestTop + 1);
            int actualTop = actualBottom - 1;
            actualStalactiteHeight = ceilingY - actualBottom;
            actualStalagmiteHeight = actualTop - floorY;
        } else {
            actualStalactiteHeight = stalactiteHeight;
            actualStalagmiteHeight = stalagmiteHeight;
        }

        bool mergeTips = random.nextBoolean() &&
            actualStalactiteHeight > 0 && actualStalagmiteHeight > 0 &&
            column.getHeight().has_value() &&
            actualStalactiteHeight + actualStalagmiteHeight == *column.getHeight();

        if (ceiling.has_value()) {
            DripstoneUtils::growPointedDripstone(level, pos.atY(*ceiling - 1),
                core::Direction::DOWN, actualStalactiteHeight, mergeTips);
        }
        if (floor.has_value()) {
            DripstoneUtils::growPointedDripstone(level, pos.atY(*floor + 1),
                core::Direction::UP, actualStalagmiteHeight, mergeTips);
        }
    }

    bool isLava(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* state = level->getBlockState(pos);
        return state && state->getIdentifier() == "minecraft:lava";
    }

    int getDripstoneHeight(WorldgenRandom& random, int dx, int dz, float density,
        int maxHeight, const DripstoneClusterConfiguration& config) {
        if (random.nextFloat() > density) return 0;
        int distFromCenter = std::abs(dx) + std::abs(dz);
        float heightMean = static_cast<float>(Mth::clampedMap(
            static_cast<double>(distFromCenter), 0.0,
            static_cast<double>(config.maxDistanceFromCenterAffectingHeightBias),
            static_cast<double>(maxHeight) / 2.0, 0.0));
        return static_cast<int>(levelgen::carver::ClampedNormalFloat::sample(
            random, heightMean, static_cast<float>(config.heightDeviation),
            0.0f, static_cast<float>(maxHeight)));
    }

    bool canPlacePool(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* state = level->getBlockState(pos);
        if (!state) return false;
        const std::string& id = state->getIdentifier();
        if (id == "minecraft:water" || id == "minecraft:dripstone_block" || id == "minecraft:pointed_dripstone")
            return false;
        // Check above not water
        BlockState* above = level->getBlockState(pos.above());
        if (above && above->getIdentifier() == "minecraft:water") return false;
        // Check horizontal neighbors and below
        core::Direction horizontals[] = {
            core::Direction::NORTH, core::Direction::EAST,
            core::Direction::SOUTH, core::Direction::WEST
        };
        for (auto dir : horizontals) {
            if (!canBeAdjacentToWater(level, pos.relative(dir))) return false;
        }
        return canBeAdjacentToWater(level, pos.below());
    }

    bool canBeAdjacentToWater(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* state = level->getBlockState(pos);
        if (!state) return false;
        return DripstoneUtils::isBaseStoneOverworld(state) || state->getIdentifier() == "minecraft:water";
    }

    void replaceBlocksWithDripstoneBlocks(WorldGenLevel* level, const core::BlockPos& firstPos,
        int maxCount, core::Direction direction) {
        core::BlockPos::MutableBlockPos pos(firstPos.getX(), firstPos.getY(), firstPos.getZ());
        for (int i = 0; i < maxCount; ++i) {
            if (!DripstoneUtils::placeDripstoneBlockIfPossible(level, pos)) return;
            pos.move(direction);
        }
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
        WorldGenLevel* level = context.level();
        core::BlockPos origin = context.origin();
        WorldgenRandom& random = context.random();

        // Find solid ground
        core::BlockPos::MutableBlockPos searchPos(origin.getX(), origin.getY(), origin.getZ());
        while (searchPos.getY() > level->getMinY() + 2) {
            BlockState* block = level->getBlockState(searchPos);
            if (block && !static_cast<BlockState*>(block)->isAir()) {
                break;
            }
            searchPos.move(0, -1, 0);
        }

        origin = core::BlockPos(searchPos.getX(), searchPos.getY(), searchPos.getZ());

        // Check if on sand
        BlockState* groundBlock = level->getBlockState(origin);
        if (!groundBlock || static_cast<BlockState*>(groundBlock)->getIdentifier() != "minecraft:sand") {
            return false;
        }

        // Check for solid ground below
        for (int ox = -2; ox <= 2; ++ox) {
            for (int oz = -2; oz <= 2; ++oz) {
                core::BlockPos checkPos1 = origin.offset(ox, -1, oz);
                core::BlockPos checkPos2 = origin.offset(ox, -2, oz);
                BlockState* b1 = level->getBlockState(checkPos1);
                BlockState* b2 = level->getBlockState(checkPos2);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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
// Reference: WeightedPlacedFeature.java
//=============================================================================

/**
 * WeightedPlacedFeature - A placed feature with probability threshold
 * Reference: WeightedPlacedFeature.java
 *
 * The chance value represents the probability that THIS feature will be selected
 * when checked in the RandomSelectorFeature iteration.
 */
struct WeightedPlacedFeature {
    placement::PlacedFeature* feature;  // The feature to potentially place
    float chance;                        // Probability threshold (0.0 to 1.0)

    WeightedPlacedFeature(placement::PlacedFeature* f, float c)
        : feature(f), chance(c) {}

    /**
     * Place the feature
     * Reference: WeightedPlacedFeature.java place() method
     */
    bool place(WorldGenLevel* level, ChunkGenerator* generator,
               WorldgenRandom& random, const core::BlockPos& origin);
};

/**
 * RandomFeatureConfiguration - Config for RandomSelectorFeature
 * Reference: RandomFeatureConfiguration.java
 */
struct RandomFeatureConfiguration {
    std::vector<WeightedPlacedFeature> features;  // Weighted features to try in order
    placement::PlacedFeature* defaultFeature;      // Fallback if none match

    RandomFeatureConfiguration()
        : defaultFeature(nullptr) {}

    RandomFeatureConfiguration(
        std::vector<WeightedPlacedFeature> f,
        placement::PlacedFeature* def
    )
        : features(std::move(f))
        , defaultFeature(def)
    {}
};

//=============================================================================
// RandomSelectorFeature
// Reference: RandomSelectorFeature.java
//=============================================================================

/**
 * RandomSelectorFeature - Randomly selects from weighted features
 * Reference: RandomSelectorFeature.java lines 16-30
 *
 * CRITICAL for parity: This feature iterates through the weighted features list
 * and for EACH feature, consumes ONE nextFloat() call to check if that feature
 * should be placed. The first feature where random.nextFloat() < chance is
 * placed, and if none match, the defaultFeature is placed.
 *
 * This is the key algorithm used by DARK_FOREST_VEGETATION to select between
 * dark oak trees, birch trees, mushrooms, etc.
 */
class RandomSelectorFeature : public Feature<RandomFeatureConfiguration> {
public:
    /**
     * Place a randomly selected feature
     * Reference: RandomSelectorFeature.java place() lines 16-30
     *
     * Java code:
     *   for(WeightedPlacedFeature feature : config.features) {
     *       if (random.nextFloat() < feature.chance) {
     *           return feature.place(level, chunkGenerator, random, origin);
     *       }
     *   }
     *   return config.defaultFeature.value().place(level, chunkGenerator, random, origin);
     */
    bool place(FeaturePlaceContext<RandomFeatureConfiguration>& context) override {
        // Reference: RandomSelectorFeature.java place() lines 16-30
        const RandomFeatureConfiguration& config = context.config();
        WorldgenRandom& random = context.random();
        WorldGenLevel* level = context.level();
        ChunkGenerator* generator = context.chunkGenerator();
        const core::BlockPos& origin = context.origin();

        // Iterate through weighted features in order
        // CRITICAL: Each iteration consumes ONE nextFloat() call
        for (const WeightedPlacedFeature& weightedFeature : config.features) {
            float roll = random.nextFloat();  // MUST consume random for each feature
            if (roll < weightedFeature.chance) {
                // This feature was selected - place it and return
                if (weightedFeature.feature) {
                    return weightedFeature.feature->place(level, generator, random, origin);
                }
                return true;  // Feature is null but was selected - treated as success
            }
        }

        // No weighted feature was selected - place the default feature
        if (config.defaultFeature) {
            return config.defaultFeature->place(level, generator, random, origin);
        }

        return true;  // No default feature - treat as success
    }
};

//=============================================================================
// RandomBooleanFeatureConfiguration
// Reference: RandomBooleanFeatureConfiguration.java
//=============================================================================

struct RandomBooleanFeatureConfiguration {
    placement::PlacedFeature* featureTrue = nullptr;
    placement::PlacedFeature* featureFalse = nullptr;

    RandomBooleanFeatureConfiguration() = default;

    RandomBooleanFeatureConfiguration(
        placement::PlacedFeature* featureTrue,
        placement::PlacedFeature* featureFalse
    )
        : featureTrue(featureTrue)
        , featureFalse(featureFalse)
    {}
};

//=============================================================================
// RandomBooleanSelectorFeature
// Reference: RandomBooleanSelectorFeature.java
//=============================================================================

class RandomBooleanSelectorFeature : public Feature<RandomBooleanFeatureConfiguration> {
public:
    // Reference: RandomBooleanSelectorFeature.java place() lines 16-24
    bool place(FeaturePlaceContext<RandomBooleanFeatureConfiguration>& context) override {
        WorldgenRandom& random = context.random();
        const RandomBooleanFeatureConfiguration& config = context.config();
        WorldGenLevel* level = context.level();
        ChunkGenerator* chunkGenerator = context.chunkGenerator();
        const core::BlockPos& origin = context.origin();
        bool result = random.nextBoolean();
        placement::PlacedFeature* selected = result ? config.featureTrue : config.featureFalse;
        return selected && selected->place(level, chunkGenerator, random, origin);
    }
};

//=============================================================================
// SimpleRandomFeatureConfiguration
// Reference: SimpleRandomFeatureConfiguration.java
//=============================================================================

struct SimpleRandomFeatureConfiguration {
    // Reference: SimpleRandomFeatureConfiguration.java line 12
    // public final HolderSet<PlacedFeature> features;
    std::vector<placement::PlacedFeature*> features;

    SimpleRandomFeatureConfiguration() = default;

    // Reference: SimpleRandomFeatureConfiguration.java lines 14-16
    explicit SimpleRandomFeatureConfiguration(const std::vector<placement::PlacedFeature*>& featureList)
        : features(featureList) {}

    // Convenience: size() method to match Java's features.size()
    size_t size() const { return features.size(); }

    // Convenience: get() method to match Java's features.get(index).value()
    placement::PlacedFeature* get(size_t index) const {
        return (index < features.size()) ? features[index] : nullptr;
    }
};

//=============================================================================
// SimpleRandomSelectorFeature
// Reference: SimpleRandomSelectorFeature.java
//=============================================================================

class SimpleRandomSelectorFeature : public Feature<SimpleRandomFeatureConfiguration> {
public:
    // Reference: SimpleRandomSelectorFeature.java place() lines 16-25
    bool place(FeaturePlaceContext<SimpleRandomFeatureConfiguration>& context) override {
        // Reference: SimpleRandomSelectorFeature.java lines 17-21
        WorldgenRandom& random = context.random();
        const SimpleRandomFeatureConfiguration& config = context.config();
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        ChunkGenerator* chunkGenerator = context.chunkGenerator();

        if (config.size() == 0) {
            return false;
        }

        // Reference: SimpleRandomSelectorFeature.java line 22
        // int index = random.nextInt(config.features.size());
        int index = random.nextInt(static_cast<int>(config.size()));

        // Reference: SimpleRandomSelectorFeature.java line 23
        // PlacedFeature feature = config.features.get(index).value();
        placement::PlacedFeature* feature = config.get(static_cast<size_t>(index));
        if (!feature) {
            return false;
        }

        // Reference: SimpleRandomSelectorFeature.java line 24
        // return feature.place(level, chunkGenerator, random, origin);
        return feature->place(level, chunkGenerator, random, origin);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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
    bool findFirstAirBlockAboveGround(WorldGenLevel* level, core::BlockPos::MutableBlockPos& pos) {
        while (pos.getY() > level->getMinY()) {
            pos.move(0, -1, 0);
            BlockState* block = level->getBlockState(pos);
            if (block && !static_cast<BlockState*>(block)->isAir()) {
                pos.move(0, 1, 0);
                return true;
            }
        }
        return false;
    }

    bool isInvalidPlacementLocation(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* block = level->getBlockState(pos);
        if (!block || !static_cast<BlockState*>(block)->isAir()) {
            return true;
        }

        BlockState* belowBlock = level->getBlockState(pos.below());
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        BlockState* originBlock = level->getBlockState(origin);
        if (!originBlock || !static_cast<BlockState*>(originBlock)->isAir()) {
            return false;
        }

        BlockState* aboveBlock = level->getBlockState(origin.above());
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
    void placeRoofNetherWart(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& origin) {
        // Would place nether wart blocks
        core::BlockPos::MutableBlockPos placePos(origin.getX(), origin.getY(), origin.getZ());
        core::BlockPos::MutableBlockPos neighbourPos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < 200; ++i) {
            int ox = random.nextInt(6) - random.nextInt(6);
            int oy = random.nextInt(2) - random.nextInt(5);
            int oz = random.nextInt(6) - random.nextInt(6);
            placePos.set(origin.getX() + ox, origin.getY() + oy, origin.getZ() + oz);

            BlockState* block = level->getBlockState(placePos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                // Would check neighbours and place nether wart block
            }
        }
    }

    void placeRoofWeepingVines(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& origin) {
        core::BlockPos::MutableBlockPos placePos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < 100; ++i) {
            int ox = random.nextInt(8) - random.nextInt(8);
            int oy = random.nextInt(2) - random.nextInt(7);
            int oz = random.nextInt(8) - random.nextInt(8);
            placePos.set(origin.getX() + ox, origin.getY() + oy, origin.getZ() + oz);

            BlockState* block = level->getBlockState(placePos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                BlockState* aboveBlock = level->getBlockState(placePos.above());
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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
    bool mayPlaceOn(WorldGenLevel* level, const core::BlockPos& pos, WorldgenRandom& random) {
        core::BlockPos below = pos.below();
        BlockState* belowBlock = level->getBlockState(below);
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

    void tryPlaceBlock(WorldGenLevel* level, const core::BlockPos& pos,
                       WorldgenRandom& random, const BlockPileConfiguration& config) {
        BlockState* block = level->getBlockState(pos);
        if (block && static_cast<BlockState*>(block)->isAir() &&
            mayPlaceOn(level, pos, random)) {
            // Would set block using config.stateProvider
        }
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
        WorldGenLevel* level = context.level();
        core::BlockPos origin = context.origin();
        WorldgenRandom& random = context.random();
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
    int heightDependentRadiusRound(WorldgenRandom& random, int yOff, int height, int width) {
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        BlockState* originBlock = level->getBlockState(origin);
        BlockState* aboveBlock = level->getBlockState(origin.above());

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
            BlockState* currentBlock = level->getBlockState(pos);
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
                        BlockState* belowBlock = level->getBlockState(tmpPos);
                        if (!belowBlock || !static_cast<BlockState*>(belowBlock)->isAir()) {
                            break;
                        }
                        basePos.move(0, -1, 0);
                        --maxDrop;
                    }

                    tmpPos.set(basePos.getX(), basePos.getY() - 1, basePos.getZ());
                    BlockState* finalBelow = level->getBlockState(tmpPos);
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
    void placeBaseHangOff(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos) {
        if (random.nextBoolean()) {
            // Would set BASALT at pos
        }
    }

    // Reference: BasaltPillarFeature.java placeHangOff() lines 86-92
    bool placeHangOff(WorldGenLevel* level, WorldgenRandom& random, const core::BlockPos& pos) {
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
        WorldGenLevel* level = context.level();
        core::BlockPos origin = context.origin();
        WorldgenRandom& random = context.random();
        const BlockStateConfiguration& config = context.config();

        // Find valid position - Reference: lines 21-28
        while (origin.getY() > level->getMinY() + 3) {
            BlockState* belowBlock = level->getBlockState(origin.below());
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        const LayerConfiguration& config = context.config();

        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        for (int dx = 0; dx < 16; ++dx) {
            for (int dz = 0; dz < 16; ++dz) {
                int x = origin.getX() + dx;
                int z = origin.getZ() + dz;
                int y = level->getMinY() + config.height;
                pos.set(x, y, z);

                BlockState* block = level->getBlockState(pos);
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
        WorldGenLevel* level = context.level();
        core::BlockPos origin = context.origin();
        WorldgenRandom& random = context.random();
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
                    BlockState* block = level->getBlockState(pos);
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
    core::BlockPos findTarget(WorldGenLevel* level, core::BlockPos::MutableBlockPos& cursor,
                              BlockState* target) {
        while (cursor.getY() > level->getMinY() + 1) {
            BlockState* block = level->getBlockState(cursor);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const ReplaceBlockConfiguration& config = context.config();

        BlockState* block = level->getBlockState(origin);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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

            BlockState* block = level->getBlockState(picklePos);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
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
                            // Set MAGMA_BLOCK at pos
                            level->setBlock(pos,
                                static_cast<BlockState*>(world::level::block::Blocks::getDefaultState("minecraft:magma_block")),
                                2);
                            ++placed;
                        }
                    }
                }
            }
        }

        return placed > 0;
    }

private:
    // Reference: UnderwaterMagmaFeature.java getFloorY() which uses Column.scan()
    // Column.scan first checks if origin is inside the column (water)
    // Reference: Column.java lines 23-24: if (!level.isStateAtPosition(pos, insideColumn)) return Optional.empty()
    int getFloorY(WorldGenLevel* level, const core::BlockPos& origin, const UnderwaterMagmaConfiguration& config) {
        // CRITICAL: First check if origin is in water - if not, return immediately
        // This is what Column.scan does in Java
        BlockState* originState = level->getBlockState(origin);
        if (!originState || originState->getIdentifier() != "minecraft:water") {
            return INT_MIN;  // Not in water, no valid floor
        }

        core::BlockPos::MutableBlockPos pos(origin.getX(), origin.getY(), origin.getZ());

        // Search down through water to find the floor
        for (int i = 0; i < config.floorSearchRange; ++i) {
            pos.move(0, -1, 0);  // Move down first
            BlockState* block = level->getBlockState(pos);
            if (!block) {
                continue;
            }

            BlockState* state = static_cast<BlockState*>(block);
            if (state->getIdentifier() != "minecraft:water") {
                // Found the floor (non-water block below water)
                return pos.getY();
            }
        }

        return INT_MIN;  // No floor found within search range
    }

    // Reference: UnderwaterMagmaFeature.java isValidPlacement() lines 53-64
    bool isValidPlacement(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* block = level->getBlockState(pos);
        if (!block) return false;

        BlockState* state = static_cast<BlockState*>(block);
        if (state->isAir() || state->getIdentifier() == "minecraft:water") {
            return false;
        }

        // Check if not visible from below
        BlockState* belowBlock = level->getBlockState(pos.below());
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
            BlockState* neighborBlock = level->getBlockState(neighborPos);
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
        ::world::IChunk* level = context.getChunkForOrigin();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const OreConfiguration& config = context.config();

        int numberOfTries = random.nextInt(config.size + 1);
        core::BlockPos::MutableBlockPos targetPos(origin.getX(), origin.getY(), origin.getZ());

        for (int i = 0; i < numberOfTries; ++i) {
            offsetTargetPos(targetPos, random, origin, std::min(i, MAX_DIST_FROM_ORIGIN));

            BlockState* block = level->getBlockState(targetPos);
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
    void offsetTargetPos(core::BlockPos::MutableBlockPos& targetPos, WorldgenRandom& random,
                         const core::BlockPos& origin, int maxDistFromOrigin) {
        int xd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        int yd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        int zd = getRandomPlacementInOneAxisRelativeToOrigin(random, maxDistFromOrigin);
        targetPos.set(origin.getX() + xd, origin.getY() + yd, origin.getZ() + zd);
    }

    // Reference: ScatteredOreFeature.java getRandomPlacementInOneAxisRelativeToOrigin() lines 49-51
    int getRandomPlacementInOneAxisRelativeToOrigin(WorldgenRandom& random, int maxDistance) {
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

    // Reference: MultifaceGrowthConfiguration.java constructor lines 50-64
    // Java adds: UP (ceiling), DOWN (floor), then HORIZONTAL (NORTH, EAST, SOUTH, WEST)
    std::vector<core::Direction> getShuffledDirections(WorldgenRandom& random) const {
        std::vector<core::Direction> directions;
        if (canPlaceOnCeiling) directions.push_back(core::Direction::UP);
        if (canPlaceOnFloor) directions.push_back(core::Direction::DOWN);
        if (canPlaceOnWall) {
            // Direction.Plane.HORIZONTAL: NORTH, EAST, SOUTH, WEST
            directions.push_back(core::Direction::NORTH);
            directions.push_back(core::Direction::EAST);
            directions.push_back(core::Direction::SOUTH);
            directions.push_back(core::Direction::WEST);
        }

        // Shuffle - Reference: Util.shuffledCopy -> Util.shuffle (Fisher-Yates)
        for (size_t i = directions.size(); i > 1; --i) {
            size_t j = random.nextInt(static_cast<int>(i));
            std::swap(directions[i - 1], directions[j]);
        }

        return directions;
    }

    // Reference: MultifaceGrowthConfiguration.java getShuffledDirectionsExcept() lines 68-70
    // Filters validDirections to exclude the given direction, then shuffles
    std::vector<core::Direction> getShuffledDirectionsExcept(WorldgenRandom& random, core::Direction excludeDirection) const {
        std::vector<core::Direction> directions;
        if (canPlaceOnCeiling && core::Direction::UP != excludeDirection)
            directions.push_back(core::Direction::UP);
        if (canPlaceOnFloor && core::Direction::DOWN != excludeDirection)
            directions.push_back(core::Direction::DOWN);
        if (canPlaceOnWall) {
            if (core::Direction::NORTH != excludeDirection) directions.push_back(core::Direction::NORTH);
            if (core::Direction::EAST != excludeDirection) directions.push_back(core::Direction::EAST);
            if (core::Direction::SOUTH != excludeDirection) directions.push_back(core::Direction::SOUTH);
            if (core::Direction::WEST != excludeDirection) directions.push_back(core::Direction::WEST);
        }

        // Shuffle - Reference: Util.toShuffledList -> Util.shuffle (Fisher-Yates)
        for (size_t i = directions.size(); i > 1; --i) {
            size_t j = random.nextInt(static_cast<int>(i));
            std::swap(directions[i - 1], directions[j]);
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const MultifaceGrowthConfiguration& config = context.config();

        BlockState* originBlock = level->getBlockState(origin);
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

            // Reference: line 34 - get new shuffled directions excluding opposite of search dir
            std::vector<core::Direction> placementDirections =
                config.getShuffledDirectionsExcept(random, core::getOpposite(searchDirection));

            for (int i = 0; i < config.searchRange; ++i) {
                pos.setWithOffset(
                    origin,
                    core::getStepX(searchDirection),
                    core::getStepY(searchDirection),
                    core::getStepZ(searchDirection)
                );
                BlockState* block = level->getBlockState(pos);
                if (!block) break;

                BlockState* state = static_cast<BlockState*>(block);
                if (!isAirOrWater(state) && state->getIdentifier() != config.placeBlock) {
                    break;
                }

                if (placeGrowthIfPossible(level, pos, state, config, random, placementDirections)) {
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
        WorldGenLevel* level,
        const core::BlockPos& pos,
        BlockState* oldState,
        const MultifaceGrowthConfiguration& config,
        WorldgenRandom& random,
        const std::vector<core::Direction>& placementDirections
    ) {
        core::BlockPos::MutableBlockPos mutPos(pos.getX(), pos.getY(), pos.getZ());

        for (const auto& placementDirection : placementDirections) {
            mutPos.set(pos.getX(), pos.getY(), pos.getZ());
            mutPos.move(placementDirection);

            BlockState* neighborBlock = level->getBlockState(mutPos);
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

            if (!canPlace) {
                continue;
            }

            Block* placeBlock = world::level::block::Blocks::getBlock(config.placeBlock);
            BlockState* newState = nullptr;
            auto* sculkVeinBlock = dynamic_cast<world::level::block::SculkVeinBlock*>(placeBlock);
            if (sculkVeinBlock) {
                newState = sculkVeinBlock->getStateForPlacement(oldState, *level, pos, placementDirection);
            } else {
                newState = static_cast<BlockState*>(world::level::block::Blocks::getDefaultState(config.placeBlock));
            }

            if (!newState) {
                return false;
            }

            level->setBlock(pos, newState, 3);
            if (::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->markPosForPostprocessing(pos);
            }

            if (random.nextFloat() < config.chanceOfSpreading) {
                if (sculkVeinBlock) {
                    sculkVeinBlock->spreadFromFaceTowardRandomDirection(
                        newState,
                        level,
                        pos,
                        placementDirection,
                        random,
                        true
                    );
                } else {
                    random.nextInt(6);
                    random.nextInt(5);
                    random.nextInt(4);
                    random.nextInt(3);
                    random.nextInt(2);
                }
            }

            return true;
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
// Uses SculkSpreader for proper block placement
//=============================================================================

class SculkPatchFeature : public Feature<SculkPatchConfiguration> {
public:
    // Reference: SculkPatchFeature.java place() lines 23-63
    bool place(FeaturePlaceContext<SculkPatchConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const SculkPatchConfiguration& config = context.config();

        if (!canSpreadFrom(level, origin)) {
            return false;
        }

        // Create world gen spreader
        // Reference: SculkSpreader.createWorldGenSpreader()
        world::level::block::SculkSpreader spreader = world::level::block::SculkSpreader::createWorldGenSpreader();

        int totalRounds = config.spreadRounds + config.growthRounds;

        // Reference: SculkPatchFeature.java lines 34-46
        for (int round = 0; round < totalRounds; ++round) {
            // Add cursors
            for (int i = 0; i < config.chargeCount; ++i) {
                spreader.addCursors(origin, config.amountPerCharge);
            }

            bool spreadVeins = round < config.spreadRounds;

            for (int i = 0; i < config.spreadAttempts; ++i) {
                spreader.updateCursors(level, origin, random, spreadVeins);
            }

            spreader.clear();
        }

        // Place catalyst - Reference: lines 48-51
        core::BlockPos below = origin.below();
        BlockState* belowBlock = level->getBlockState(below);
        if (random.nextFloat() <= config.catalystChance && belowBlock &&
            static_cast<BlockState*>(belowBlock)->isCollisionShapeFullBlock(*level, below)) {
            level->setBlock(origin,
                static_cast<BlockState*>(world::level::block::Blocks::getDefaultState("minecraft:sculk_catalyst")),
                3);
        }

        // Place extra shrieker growths - Reference: lines 53-60
        int extraGrowths = config.extraRareGrowths ? config.extraRareGrowths->sample(random) : 0;
        for (int i = 0; i < extraGrowths; ++i) {
            core::BlockPos candidate = origin.offset(random.nextInt(5) - 2, 0, random.nextInt(5) - 2);
            BlockState* candBlock = level->getBlockState(candidate);
            core::BlockPos candBelow = candidate.below();
            BlockState* candBelowBlock = level->getBlockState(candBelow);
            if (candBlock && static_cast<BlockState*>(candBlock)->isAir() &&
                candBelowBlock && static_cast<BlockState*>(candBelowBlock)->isFaceSturdy(*level, candBelow, core::Direction::UP)) {
                BlockState* shriekerState = static_cast<BlockState*>(
                    world::level::block::Blocks::getDefaultState("minecraft:sculk_shrieker")
                );
                if (shriekerState && BlockStateProperties::CAN_SUMMON &&
                    shriekerState->hasProperty(BlockStateProperties::CAN_SUMMON)) {
                    shriekerState = shriekerState->setValue(*BlockStateProperties::CAN_SUMMON, true);
                }
                level->setBlock(candidate,
                    shriekerState,
                    3);
            }
        }

        return true;
    }

private:
    // Reference: SculkPatchFeature.java canSpreadFrom() lines 66-77
    bool canSpreadFrom(WorldGenLevel* level, const core::BlockPos& origin) {
        BlockState* block = level->getBlockState(origin);
        if (!block) return false;

        BlockState* state = static_cast<BlockState*>(block);

        if (dynamic_cast<world::level::block::SculkBehaviour*>(state->getBlock()) != nullptr) {
            return true;
        }

        if (!state->isAir() && state->getIdentifier() != "minecraft:water") {
            return false;
        }

        // Check if any neighbor is solid
        for (int dir = 0; dir < 6; ++dir) {
            core::Direction direction = blockpredicates::fromIndex(dir);
            core::BlockPos neighborPos = origin.relative(direction);
            BlockState* neighborBlock = level->getBlockState(neighborPos);
            if (neighborBlock &&
                static_cast<BlockState*>(neighborBlock)->isCollisionShapeFullBlock(*level, neighborPos)) {
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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const RootSystemConfiguration& config = context.config();

        BlockState* originBlock = level->getBlockState(origin);
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
    void placeRoots(WorldGenLevel* level, const RootSystemConfiguration& config,
                    WorldgenRandom& random, const core::BlockPos& pos,
                    core::BlockPos::MutableBlockPos& workingPos) {
        int rootRadius = config.hangingRootRadius;
        int verticalSpan = config.hangingRootsVerticalSpan;

        for (int i = 0; i < config.hangingRootPlacementAttempts; ++i) {
            int ox = random.nextInt(rootRadius) - random.nextInt(rootRadius);
            int oy = random.nextInt(verticalSpan) - random.nextInt(verticalSpan);
            int oz = random.nextInt(rootRadius) - random.nextInt(rootRadius);
            workingPos.set(pos.getX() + ox, pos.getY() + oy, pos.getZ() + oz);

            BlockState* block = level->getBlockState(workingPos);
            if (block && static_cast<BlockState*>(block)->isAir()) {
                BlockState* aboveBlock = level->getBlockState(workingPos.above());
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
    util::JavaHashSet<core::BlockPos> placeGroundPatch(
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        int xRadius,
        int zRadius
    ) override {
        util::JavaHashSet<core::BlockPos> surface =
            VegetationPatchFeature::placeGroundPatch(level, config, random, origin, xRadius, zRadius);
        util::JavaHashSet<core::BlockPos> waterSurface;
        core::BlockPos::MutableBlockPos testPos(0, 0, 0);

        for (const core::BlockPos& surfacePos : surface) {
            if (!isExposed(*level, surfacePos, testPos)) {
                waterSurface.add(surfacePos);
            }
        }

        for (const core::BlockPos& surfacePos : waterSurface) {
            level->setBlock(surfacePos, world::level::block::Blocks::WATER->defaultBlockState(), 2);
        }

        return waterSurface;
    }

    bool placeVegetation(
        WorldGenLevel* level,
        const VegetationPatchConfiguration& config,
        ChunkGenerator* generator,
        WorldgenRandom& random,
        const core::BlockPos& placementPos
    ) override {
        if (VegetationPatchFeature::placeVegetation(level, config, generator, random, placementPos.below())) {
            BlockState* placed = level->getBlockState(placementPos);
            if (placed && placed->hasProperty(BlockStateProperties::WATERLOGGED) &&
                !placed->getValue(*BlockStateProperties::WATERLOGGED)) {
                level->setBlock(placementPos, placed->setValue(*BlockStateProperties::WATERLOGGED, true), 2);
            }
            return true;
        }

        return false;
    }

private:
    static bool isExposed(
        const WorldGenLevel& level,
        const core::BlockPos& pos,
        core::BlockPos::MutableBlockPos& testPos
    ) {
        return isExposedDirection(level, pos, testPos, core::Direction::NORTH) ||
               isExposedDirection(level, pos, testPos, core::Direction::EAST) ||
               isExposedDirection(level, pos, testPos, core::Direction::SOUTH) ||
               isExposedDirection(level, pos, testPos, core::Direction::WEST) ||
               isExposedDirection(level, pos, testPos, core::Direction::DOWN);
    }

    static bool isExposedDirection(
        const WorldGenLevel& level,
        const core::BlockPos& pos,
        core::BlockPos::MutableBlockPos& testPos,
        core::Direction direction
    ) {
        testPos.setWithOffset(pos, core::getStepX(direction), core::getStepY(direction), core::getStepZ(direction));
        BlockState* state = level.getBlockState(testPos);
        return !state || !state->isFaceSturdy(level, testPos, core::getOpposite(direction));
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
        WorldGenLevel* level = context.level();
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
private:
    // Mob types: SKELETON, ZOMBIE, ZOMBIE, SPIDER (weighted)
    // Reference: MonsterRoomFeature.java lines 134-136
    static constexpr const char* MOBS[] = {
        "minecraft:skeleton", "minecraft:zombie", "minecraft:zombie", "minecraft:spider"
    };

public:
    // Reference: MonsterRoomFeature.java place() lines 32-128
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

        // Room dimensions - Reference: lines 37-45
        int xr = random.nextInt(2) + 2;  // 2-3
        int zr = random.nextInt(2) + 2;  // 2-3
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
                    BlockState* state = level->getBlockState(holePos);
                    if (!state) continue;

                    bool solid = !state->isAir();

                    // Floor must be solid
                    if (dy == -1 && !solid) return false;
                    // Ceiling must be solid
                    if (dy == 4 && !solid) return false;

                    // Count entrance holes (walls at y=0 with empty above)
                    if ((dx == minX || dx == maxX || dz == minZ || dz == maxZ) && dy == 0) {
                        if (state->isAir()) {
                            BlockState* aboveState = level->getBlockState(holePos.above());
                            if (aboveState && aboveState->isAir()) {
                                ++holeCount;
                            }
                        }
                    }
                }
            }
        }

        // Must have 1-5 entrance holes - Reference: line 68
        if (holeCount < 1 || holeCount > 5) {
            return false;
        }

        // Place dungeon structure - Reference: lines 69-89
        BlockState* caveAir = world::level::block::Blocks::CAVE_AIR->defaultBlockState();
        BlockState* cobblestone = world::level::block::Blocks::COBBLESTONE->defaultBlockState();
        BlockState* mossyCobblestone = world::level::block::Blocks::MOSSY_COBBLESTONE->defaultBlockState();

        for (int dx = minX; dx <= maxX; ++dx) {
            for (int dy = 3; dy >= -1; --dy) {
                for (int dz = minZ; dz <= maxZ; ++dz) {
                    core::BlockPos wallPos = origin.offset(dx, dy, dz);
                    BlockState* wallState = level->getBlockState(wallPos);

                    // Check if interior (not on walls/floor/ceiling)
                    bool isInterior = dx != minX && dy != -1 && dz != minZ &&
                                      dx != maxX && dy != 4 && dz != maxZ;

                    if (isInterior) {
                        // Interior: set to cave air (skip if chest or spawner)
                        if (wallState && !wallState->is(world::level::block::Blocks::CHEST) &&
                            !wallState->is(world::level::block::Blocks::SPAWNER)) {
                            level->setBlock(wallPos, caveAir, 2);
                        }
                    } else {
                        // Wall/floor/ceiling: check if should place cobblestone
                        BlockState* belowState = level->getBlockState(wallPos.below());
                        if (belowState && !belowState->isAir()) {
                            // Has solid support
                            if (wallState && !wallState->isAir() &&
                                !wallState->is(world::level::block::Blocks::CHEST)) {
                                // Floor: 75% mossy cobblestone, 25% cobblestone
                                if (dy == -1 && random.nextInt(4) != 0) {
                                    level->setBlock(wallPos, mossyCobblestone, 2);
                                } else {
                                    level->setBlock(wallPos, cobblestone, 2);
                                }
                            }
                        } else {
                            // No support below - set to air
                            level->setBlock(wallPos, caveAir, 2);
                        }
                    }
                }
            }
        }

        // Place chests - Reference: lines 91-113
        // Two iterations, each tries up to 3 positions
        for (int chestIter = 0; chestIter < 2; ++chestIter) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                int xc = origin.getX() + random.nextInt(xr * 2 + 1) - xr;
                int yc = origin.getY();
                int zc = origin.getZ() + random.nextInt(zr * 2 + 1) - zr;
                core::BlockPos chestPos(xc, yc, zc);

                BlockState* chestPosState = level->getBlockState(chestPos);
                if (chestPosState && chestPosState->isAir()) {
                    // Count adjacent solid walls
                    int wallCount = 0;
                    for (const core::Direction& dir : {core::Direction::NORTH, core::Direction::SOUTH,
                                                       core::Direction::EAST, core::Direction::WEST}) {
                        core::BlockPos adjPos = chestPos.relative(dir, 1);
                        BlockState* adjState = level->getBlockState(adjPos);
                        if (adjState && !adjState->isAir()) {
                            ++wallCount;
                        }
                    }

                    // Place chest if exactly 1 adjacent wall
                    if (wallCount == 1) {
                        level->setBlock(chestPos,
                            world::level::block::Blocks::CHEST->defaultBlockState(), 2);
                        // Note: Loot table would be set on block entity in full implementation
                        break;  // Success, move to next iteration
                    }
                }
            }
        }

        // Place spawner at center - Reference: lines 115-122
        level->setBlock(origin,
            world::level::block::Blocks::SPAWNER->defaultBlockState(), 2);
        // Note: Spawner entity mob type would be set here:
        // randomEntityId = MOBS[random.nextInt(4)]

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
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();

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

                BlockState* block = level->getBlockState(chestPos);
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

struct FallenTreeConfiguration : public FeatureConfiguration {
    std::shared_ptr<feature::stateproviders::BlockStateProvider> trunkProvider;
    std::shared_ptr<util::IntProvider> logLength;
    std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> stumpDecorators;
    std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> logDecorators;

    FallenTreeConfiguration() = default;

    FallenTreeConfiguration(
        std::shared_ptr<feature::stateproviders::BlockStateProvider> trunk,
        std::shared_ptr<util::IntProvider> length,
        std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> stumpDeco = {},
        std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>> logDeco = {}
    ) : trunkProvider(std::move(trunk))
      , logLength(std::move(length))
      , stumpDecorators(std::move(stumpDeco))
      , logDecorators(std::move(logDeco))
    {}
};

//=============================================================================
// FallenTreeFeature
// Reference: FallenTreeFeature.java
//
// CRITICAL RANDOM CONSUMPTION ORDER (for bit parity):
// 1. placeStump() -> trunkProvider.getState(random) -> stumpDecorators
// 2. Direction.Plane.HORIZONTAL.getRandomDirection(random) -> random.nextInt(4)
// 3. config.logLength.sample(random)
// 4. random.nextInt(2) for start offset
// 5. placeFallenLog() -> trunkProvider.getState(random) per block -> logDecorators
//=============================================================================

class FallenTreeFeature : public Feature<FallenTreeConfiguration> {
private:
    static constexpr int STUMP_HEIGHT = 1;
    static constexpr int STUMP_HEIGHT_PLUS_EMPTY_SPACE = 2;
    static constexpr int FALLEN_LOG_MAX_FALL_HEIGHT_TO_GROUND = 5;
    static constexpr int FALLEN_LOG_MAX_GROUND_GAP = 2;
    static constexpr int FALLEN_LOG_MAX_SPACE_FROM_STUMP = 2;

public:
    // Reference: FallenTreeFeature.java place() lines 30-33
    bool place(FeaturePlaceContext<FallenTreeConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();
        WorldgenRandom& random = context.random();
        const FallenTreeConfiguration& config = context.config();

        placeFallenTree(config, origin, level, random);
        return true;
    }

private:
    // Reference: FallenTreeFeature.java placeFallenTree() lines 35-44
    // CRITICAL: Order of operations must match Java exactly
    void placeFallenTree(const FallenTreeConfiguration& config, const core::BlockPos& origin,
                         WorldGenLevel* level, WorldgenRandom& random) {
        // Step 1: Place stump FIRST (consumes random for block state + decorators)
        core::BlockPos::MutableBlockPos stumpPos(origin.getX(), origin.getY(), origin.getZ());
        placeStump(config, level, random, stumpPos);

        // Step 2: Get random direction - random.nextInt(4)
        core::Direction direction = blockpredicates::fromHorizontalIndex(random.nextInt(4));

        // Step 3: Sample log length
        int logLength = config.logLength ? config.logLength->sample(random) - 2 : 3;

        // Step 4: Calculate log start position with random offset
        int startOffset = 2 + random.nextInt(2);
        core::BlockPos::MutableBlockPos logStartPos(
            origin.getX() + blockpredicates::getStepX(direction) * startOffset,
            origin.getY(),
            origin.getZ() + blockpredicates::getStepZ(direction) * startOffset
        );

        // Set ground height for fallen log (no random consumption)
        setGroundHeightForFallenLogStartPos(level, logStartPos);

        // Step 5: Check and place fallen log (consumes random for block states + decorators)
        if (canPlaceEntireFallenLog(level, logLength, logStartPos, direction)) {
            placeFallenLog(config, level, random, logLength, logStartPos, direction);
        }
    }

    // Reference: FallenTreeFeature.java placeStump() lines 60-63
    void placeStump(const FallenTreeConfiguration& config, WorldGenLevel* level,
                    WorldgenRandom& random, core::BlockPos::MutableBlockPos& stumpPos) {
        // Place single stump log block (upright, no axis modification)
        core::BlockPos stump = placeLogBlock(config, level, random, stumpPos,
            [](BlockState* s) { return s; }  // identity - no axis change for stump
        );

        // Decorate stump
        decorateLogs(level, random, {stump}, config.stumpDecorators);
    }

    // Reference: FallenTreeFeature.java placeLogBlock() lines 108-112
    core::BlockPos placeLogBlock(const FallenTreeConfiguration& config, WorldGenLevel* level,
                                  WorldgenRandom& random, const core::BlockPos& pos,
                                  std::function<BlockState*(BlockState*)> stateModifier) {
        // Get block state from provider (CONSUMES RANDOM)
        BlockState* state = config.trunkProvider->getState(random, pos);

        // Apply state modifier (e.g., set AXIS for fallen logs)
        state = stateModifier(state);

        // Place the block
        level->setBlock(pos, state, 3);

        // Mark above for post processing
        markAboveForPostProcessing(level, pos);

        return core::BlockPos(pos.getX(), pos.getY(), pos.getZ());
    }

    // Reference: FallenTreeFeature.java decorateLogs() lines 114-119
    void decorateLogs(WorldGenLevel* level, WorldgenRandom& random,
                      const std::set<core::BlockPos>& logs,
                      const std::vector<std::shared_ptr<feature::treedecorators::TreeDecorator>>& decorators) {
        if (decorators.empty() || logs.empty()) {
            return;
        }

        // Convert set to vector for decorator context
        std::vector<core::BlockPos> logsVec(logs.begin(), logs.end());

        // Create decoration setter lambda (Reference: FallenTreeFeature.java getDecorationSetter)
        auto decorationSetter = [level](const core::BlockPos& pos, BlockState* state) {
            level->setBlock(pos, state, 19);  // flags = 19 like Java
        };

        // Create block getter lambda
        auto blockGetter = [level](const core::BlockPos& pos) -> BlockState* {
            return level->getBlockState(pos);
        };

        // Heightmap getter for MOTION_BLOCKING_NO_LEAVES
        // Reference: PlaceOnGroundDecorator.java line 68
        auto heightGetter = [level](int x, int z) -> int {
            return level->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
        };

        // Create decorator context with only logs (no leaves or roots for fallen trees)
        feature::treedecorators::DecoratorContext context(
            logsVec,
            std::vector<core::BlockPos>{},  // no leaves
            std::vector<core::BlockPos>{},  // no roots
            decorationSetter,
            blockGetter,
            heightGetter,
            &random
        );

        // Apply each decorator
        for (const auto& decorator : decorators) {
            if (decorator) {
                decorator->place(context);
            }
        }
    }

    // Reference: FallenTreeFeature.java setGroundHeightForFallenLogStartPos() lines 47-57
    void setGroundHeightForFallenLogStartPos(WorldGenLevel* level, core::BlockPos::MutableBlockPos& pos) {
        pos.move(0, 1, 0);  // Move up by 1
        for (int i = 0; i < 6; ++i) {
            if (mayPlaceOn(level, pos)) {
                return;
            }
            pos.move(0, -1, 0);  // Move down
        }
    }

    // Reference: FallenTreeFeature.java canPlaceEntireFallenLog() lines 65-87
    bool canPlaceEntireFallenLog(WorldGenLevel* level, int logLength,
                                  core::BlockPos::MutableBlockPos& pos, core::Direction direction) {
        int gapInGround = 0;

        for (int i = 0; i < logLength; ++i) {
            // Check if position is valid for tree (uses TreeFeature.validTreePos logic)
            BlockState* block = level->getBlockState(pos);
            if (!block) return false;

            if (!block->isAir() && !block->isLeaves()) {
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

        // Move back to start position
        pos.move(blockpredicates::opposite(direction), logLength);

        return true;
    }

    // Reference: FallenTreeFeature.java placeFallenLog() lines 89-98
    void placeFallenLog(const FallenTreeConfiguration& config, WorldGenLevel* level,
                        WorldgenRandom& random, int logLength,
                        core::BlockPos::MutableBlockPos& pos, core::Direction direction) {
        std::set<core::BlockPos> fallenLog;

        // Create state modifier that sets AXIS based on direction
        auto sidewaysModifier = getSidewaysStateModifier(direction);

        for (int i = 0; i < logLength; ++i) {
            // Place log block with horizontal axis
            core::BlockPos placed = placeLogBlock(config, level, random, pos, sidewaysModifier);
            fallenLog.insert(placed);
            pos.move(direction);
        }

        // Decorate fallen log
        decorateLogs(level, random, fallenLog, config.logDecorators);
    }

    // Reference: FallenTreeFeature.java getSidewaysStateModifier() lines 126-128
    static std::function<BlockState*(BlockState*)> getSidewaysStateModifier(core::Direction direction) {
        return [direction](BlockState* state) -> BlockState* {
            // Set AXIS property based on direction
            // Reference: Direction.getAxis() - NORTH/SOUTH -> Z axis, EAST/WEST -> X axis
            core::Axis axis = core::getAxis(direction);

            // Set the AXIS property on the block state
            // Reference: BlockState.trySetValue(RotatedPillarBlock.AXIS, direction.getAxis())
            // Use trySetValue to avoid throwing if block doesn't have AXIS property
            using namespace world::level::block::state::properties;
            if (BlockStateProperties::AXIS) {
                return state->trySetValue(*BlockStateProperties::AXIS, axis);
            }
            return state;
        };
    }

    // Reference: FallenTreeFeature.java mayPlaceOn() lines 100-102
    bool mayPlaceOn(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* block = level->getBlockState(pos);
        if (!block) return false;

        // validTreePos: must be air or leaves
        if (!block->isAir() && !block->isLeaves()) {
            return false;
        }

        return isOverSolidGround(level, pos);
    }

    // Reference: FallenTreeFeature.java isOverSolidGround() lines 104-106
    bool isOverSolidGround(WorldGenLevel* level, const core::BlockPos& pos) {
        BlockState* belowBlock = level->getBlockState(pos.below());
        if (!belowBlock) return false;
        // isFaceSturdy check - simplified to !isAir for now
        return !belowBlock->isAir();
    }

    // Reference: Feature.java markAboveForPostProcessing() lines 187-199
    void markAboveForPostProcessing(WorldGenLevel* level, const core::BlockPos& pos) {
        core::BlockPos::MutableBlockPos checkPos(pos.getX(), pos.getY(), pos.getZ());
        for (int i = 0; i < 2; ++i) {
            checkPos.move(0, 1, 0);
            BlockState* state = level->getBlockState(checkPos);
            if (state && state->isAir()) {
                return;
            }
            // Would mark for post-processing - simplified for now
        }
    }
};

//=============================================================================
// SnowAndFreezeFeature (FreezeTopLayerFeature)
// Reference: SnowAndFreezeFeature.java
//
// Places snow layers and freezes water in cold biomes.
// This is the TOP_LAYER_MODIFICATION feature.
//=============================================================================

class SnowAndFreezeFeature : public Feature<NoneFeatureConfiguration> {
public:
    /**
     * Place snow and freeze water in cold biomes
     * Reference: SnowAndFreezeFeature.java place() lines 18-43
     */
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        const core::BlockPos& origin = context.origin();

        core::BlockPos::MutableBlockPos topPos;
        core::BlockPos::MutableBlockPos belowPos;

        // Reference: SnowAndFreezeFeature.java lines 23-42
        for (int dx = 0; dx < 16; ++dx) {
            for (int dz = 0; dz < 16; ++dz) {
                int x = origin.getX() + dx;
                int z = origin.getZ() + dz;

                // Get height at MOTION_BLOCKING (or WORLD_SURFACE)
                int y = level->getHeight(Heightmap::Types::MOTION_BLOCKING, x, z) + 1;

                topPos.set(x, y, z);
                belowPos.set(x, y - 1, z);

                // Get biome at this position using BlockPos
                // Note: BiomeHolder is typedef for const Biome*
                const world::biome::Biome* biome = level->getBiome(topPos);

                if (!biome) continue;

                // Check temperature - cold biomes have temp < 0.15
                // Reference: Biome.shouldFreeze() / shouldSnow()
                float temperature = biome->getTemperature(topPos);

                // Reference: Biome.shouldFreeze() - freeze water if temp < 0.15
                if (temperature < 0.15f) {
                    BlockState* belowState = level->getBlockState(belowPos);
                    if (belowState && belowState->getIdentifier() == "minecraft:water") {
                        // Freeze water to ice
                        level->setBlock(belowPos,
                            static_cast<BlockState*>(world::level::block::Blocks::getDefaultState("minecraft:ice")),
                            2);
                    }
                }

                // Reference: Biome.shouldSnow() - place snow if temp < 0.15 and not in water
                if (temperature < 0.15f) {
                    BlockState* topState = level->getBlockState(topPos);
                    BlockState* belowState = level->getBlockState(belowPos);

                    // Only place snow if air above and solid below
                    if (topState && topState->isAir() && belowState && !belowState->isAir() &&
                        belowState->getIdentifier() != "minecraft:water" &&
                        belowState->getIdentifier() != "minecraft:ice") {
                        // Place snow layer
                        level->setBlock(topPos,
                            static_cast<BlockState*>(world::level::block::Blocks::getDefaultState("minecraft:snow")),
                            2);
                    }
                }
            }
        }

        return true;
    }
};

//=============================================================================
// ForestRockFeature
// Reference: ForestRockFeature.java
//
// Places a small boulder of mossy cobblestone in forested biomes.
// Uses BlockStateConfiguration to specify the block type.
//=============================================================================

class ForestRockFeature : public Feature<BlockStateConfiguration> {
public:
    /**
     * Place a small mossy cobblestone boulder
     * Reference: ForestRockFeature.java place() lines 15-42
     */
    bool place(FeaturePlaceContext<BlockStateConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        core::BlockPos origin = context.origin();
        WorldgenRandom& random = context.random();
        const BlockStateConfiguration& config = context.config();

        // Find valid position - scan down to find ground
        // Reference: ForestRockFeature.java lines 18-25
        while (origin.getY() > level->getMinY() + 3) {
            BlockState* belowBlock = level->getBlockState(origin.below());
            if (belowBlock && !belowBlock->isAir()) {
                std::string id = belowBlock->getIdentifier();
                if (id == "minecraft:dirt" || id == "minecraft:grass_block" ||
                    id == "minecraft:stone" || id == "minecraft:coarse_dirt" ||
                    id == "minecraft:podzol" || id == "minecraft:rooted_dirt") {
                    break;
                }
            }
            origin = origin.below();
        }

        if (origin.getY() <= level->getMinY() + 3) {
            return false;
        }

        // Place 3 blob iterations - Reference: ForestRockFeature.java lines 30-42
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
                            BlockState* existingBlock = level->getBlockState(blockPos);
                            if (existingBlock && existingBlock->isAir()) {
                                level->setBlock(blockPos, config.state, 2);
                            }
                        }
                    }
                }
            }

            // Move origin for next iteration
            origin = origin.offset(
                -(1 + random.nextInt(2)),
                -random.nextInt(2),
                -(1 + random.nextInt(2))
            );
        }

        return true;
    }
};

} // namespace levelgen
} // namespace minecraft
