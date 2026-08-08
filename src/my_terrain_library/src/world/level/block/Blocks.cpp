#include "world/level/block/Blocks.h"
#include "world/level/block/blocks/AmethystClusterBlock.h"
#include "world/level/block/blocks/BuddingAmethystBlock.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

namespace {

using state::BlockState;
using state::StateDefinition;
using state::properties::BlockStateProperties;
using state::properties::BooleanProperty;
using state::properties::DirectionProperty;
using state::properties::IntegerProperty;

class DryVegetationBlock : public BushBlock {
public:
    explicit DryVegetationBlock(const Block::Properties& properties)
        : BushBlock(properties) {}

protected:
    bool mayPlaceOn(BlockState* stateBelow) const override {
        return minecraft::levelgen::blockpredicates::matchesBlockTagName(
            stateBelow,
            "minecraft:dry_vegetation_may_place_on"
        );
    }
};

// Reference: BaseCoralPlantBlock / BaseCoralFanBlock - WATERLOGGED, default true.
class WaterloggedDefaultTrueBlockImpl : public Block {
public:
    explicit WaterloggedDefaultTrueBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::WATERLOGGED, true));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::WATERLOGGED);
    }
};

// Reference: MangrovePropaguleBlock - age/stage/hanging/waterlogged.
class MangrovePropaguleBlockImpl : public Block {
public:
    explicit MangrovePropaguleBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::AGE_4, 0);
            defaultState = defaultState->setValue(*BlockStateProperties::STAGE, 0);
            defaultState = defaultState->setValue(*BlockStateProperties::HANGING, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

    // Reference: MangrovePropaguleBlock.canSurvive - hanging: above must be
    // mangrove_leaves; grounded: BushBlock mayPlaceOn (dirt tag/farmland) or clay.
    bool canSurvive(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        namespace bp = minecraft::levelgen::blockpredicates;
        if (state && state->hasProperty(BlockStateProperties::HANGING) &&
            state->getValueOrElse(*BlockStateProperties::HANGING, false)) {
            BlockState* above = level.getBlockState(pos.above());
            return above && above->getIdentifier() == "minecraft:mangrove_leaves";
        }
        BlockState* below = level.getBlockState(pos.below());
        if (!below) return false;
        return bp::matchesBlockTagName(below, "minecraft:dirt") ||
               below->getIdentifier() == "minecraft:farmland" ||
               below->getIdentifier() == "minecraft:clay";
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_4, BlockStateProperties::STAGE,
                    BlockStateProperties::HANGING, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: MangroveRootsBlock - WATERLOGGED, default false.
class WaterloggedDefaultFalseBlockImpl : public Block {
public:
    explicit WaterloggedDefaultFalseBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::WATERLOGGED, false));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::WATERLOGGED);
    }
};

// Reference: BaseCoralWallFanBlock - HORIZONTAL_FACING (north) + WATERLOGGED (true).
class CoralWallFanBlockImpl : public Block {
public:
    explicit CoralWallFanBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, true);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: SeaPickleBlock.java - PICKLES 1-4 (default 1) + WATERLOGGED (true).
class SeaPickleBlockImpl : public Block {
public:
    explicit SeaPickleBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::PICKLES, 1);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, true);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::PICKLES, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: CactusBlock.java - AGE 0-15 (default 0). Worldgen-relevant
// canSurvive: every horizontal neighbour must be non-solid and not lava,
// below must be cactus or #sand, and above must not be a liquid block.
class CactusBlockImpl : public Block {
public:
    explicit CactusBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::AGE_15, 0));
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        for (int i = 0; i < 4; ++i) {
            const core::Direction dir = core::horizontalPlaneDirection(i);
            BlockState* neighbor = level.getBlockState(pos.relative(dir));
            if (neighbor &&
                (neighbor->isSolid() || neighbor->getIdentifier() == "minecraft:lava")) {
                return false;
            }
        }

        BlockState* below = level.getBlockState(pos.below());
        if (!below) {
            return false;
        }
        const bool validBelow =
            below->getIdentifier() == "minecraft:cactus" ||
            minecraft::levelgen::blockpredicates::matchesBlockTagName(below, "minecraft:sand");
        if (!validBelow) {
            return false;
        }

        BlockState* above = level.getBlockState(pos.above());
        return !(above && above->isFluid());
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_15);
    }
};

class CactusFlowerBlockImpl : public BushBlock {
public:
    explicit CactusFlowerBlockImpl(const Block::Properties& properties)
        : BushBlock(properties) {}

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        const core::BlockPos belowPos = pos.below();
        BlockState* stateBelow = level.getBlockState(belowPos);
        if (!stateBelow) {
            return false;
        }

        const std::string& name = stateBelow->getIdentifier();
        return name == "minecraft:cactus" ||
               name == "minecraft:farmland" ||
               Block::canSupportCenter(level, belowPos, core::Direction::UP);
    }
};

class SweetBerryBushBlockImpl : public BushBlock {
public:
    static inline IntegerProperty* AGE = nullptr;

    explicit SweetBerryBushBlockImpl(const Block::Properties& properties)
        : BushBlock(properties) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*AGE, 0);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(AGE);
    }

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = BlockStateProperties::AGE_3;
        }
    }
};

class WaterlilyBlockImpl : public Block {
public:
    explicit WaterlilyBlockImpl(const Block::Properties& properties)
        : Block(Block::Properties(properties).noCollission().noOcclusion()) {}

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* below = level.getBlockState(pos.below());
        BlockState* above = level.getBlockState(pos.above());
        if (!below) {
            return false;
        }

        const std::string& belowId = below->getIdentifier();
        const bool onWaterOrIce = below->hasWaterFluid() || belowId == "minecraft:ice";
        const bool fluidAboveEmpty = !above || !above->hasAnyFluid();
        return onWaterOrIce && fluidAboveEmpty;
    }
};

class SugarCaneBlockImpl : public Block {
public:
    static inline IntegerProperty* AGE = nullptr;

    explicit SugarCaneBlockImpl(const Block::Properties& properties)
        : Block(Block::Properties(properties).noCollission()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*AGE, 0);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* stateBelow = level.getBlockState(pos.below());
        if (!stateBelow) {
            return false;
        }

        if (stateBelow->is(this)) {
            return true;
        }

        if (!minecraft::levelgen::blockpredicates::matchesBlockTagName(stateBelow, "minecraft:dirt") &&
            !minecraft::levelgen::blockpredicates::matchesBlockTagName(stateBelow, "minecraft:sand")) {
            return false;
        }

        const core::BlockPos below = pos.below();
        for (int directionIndex = 0; directionIndex < 4; ++directionIndex) {
            const core::Direction direction = core::fromHorizontalIndex(directionIndex);
            const core::BlockPos adjacent = below.relative(direction);
            BlockState* adjacentState = level.getBlockState(adjacent);
            if (level.isWaterAt(adjacent) ||
                (adjacentState && adjacentState->getIdentifier() == "minecraft:frosted_ice")) {
                return true;
            }
        }

        return false;
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(AGE);
    }

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = BlockStateProperties::AGE_15;
        }
    }
};

class FireflyBushBlockImpl : public BushBlock {
public:
    explicit FireflyBushBlockImpl(const Block::Properties& properties)
        : BushBlock(properties) {}
};

class PaleHangingMossBlockImpl : public Block {
public:
    static inline BooleanProperty* TIP = nullptr;

    explicit PaleHangingMossBlockImpl(const Block::Properties& properties)
        : Block(Block::Properties(properties).noCollission().replaceableByTrees()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*TIP, true);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        const core::BlockPos abovePos = pos.above();
        BlockState* aboveState = level.getBlockState(abovePos);
        return aboveState &&
               (aboveState->is(this) ||
                aboveState->isFaceSturdy(level, abovePos, core::Direction::DOWN));
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(TIP);
    }

private:
    static void initializeProperties() {
        if (!TIP) {
            BlockStateProperties::initialize();
            TIP = BlockStateProperties::TIP;
        }
    }
};

class CocoaBlockImpl : public Block {
public:
    static inline IntegerProperty* AGE = nullptr;
    static inline DirectionProperty* FACING = nullptr;

    explicit CocoaBlockImpl(const Block::Properties& properties)
        : Block(Block::Properties(properties).noCollission()) {
        initializeProperties();
        rebuildStateDefinition();

        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*AGE, 0);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* state,
        const minecraft::levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        if (!state || !FACING || !state->hasProperty(FACING)) {
            return false;
        }

        const core::Direction facing = state->getValueOrElse(*FACING, core::Direction::NORTH);
        BlockState* supportState = level.getBlockState(pos.relative(facing));
        if (!supportState) {
            return false;
        }

        const std::string& id = supportState->getIdentifier();
        return id == "minecraft:jungle_log" ||
               id == "minecraft:jungle_wood" ||
               id == "minecraft:stripped_jungle_log" ||
               id == "minecraft:stripped_jungle_wood";
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        initializeProperties();
        builder.add(FACING, AGE);
    }

private:
    static void initializeProperties() {
        if (!AGE) {
            BlockStateProperties::initialize();
            AGE = BlockStateProperties::AGE_2;
            FACING = BlockStateProperties::HORIZONTAL_FACING;
        }
    }
};

/**
 * Single-property blocks whose Java counterparts always serialize a state
 * property that worldgen leaves at its default. Modeling the property makes
 * the canonical full-state dump match Java's StateHolder.toString() output.
 */

// Reference: LiquidBlock.java - LEVEL (0-15), default 0 (source block)
class WorldgenLiquidBlock : public Block {
public:
    explicit WorldgenLiquidBlock(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::LEVEL, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LEVEL);
    }
};

// Reference: SnowyDirtBlock.java - SNOWY, default false (grass_block, podzol, mycelium)
class SnowyDirtBlockImpl : public Block {
public:
    explicit SnowyDirtBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::SNOWY, false));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::SNOWY);
    }
};

// Reference: RedStoneOreBlock.java - LIT, default false
class RedStoneOreBlockImpl : public Block {
public:
    explicit RedStoneOreBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::LIT, false));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LIT);
    }
};

// Reference: ChestBlock.java - FACING (horizontal, default north),
// TYPE (default single), WATERLOGGED (default false)
class ChestBlockImpl : public Block {
public:
    explicit ChestBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::CHEST_TYPE, state::properties::ChestType(state::properties::ChestType::SINGLE));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::CHEST_TYPE, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: CreakingHeartBlock.java - AXIS (default Y),
// CREAKING_HEART_STATE (default uprooted), NATURAL (default false)
class CreakingHeartBlockImpl : public Block {
public:
    explicit CreakingHeartBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::AXIS, core::Axis::Y);
            defaultState = defaultState->setValue(*BlockStateProperties::CREAKING_HEART_STATE,
                state::properties::CreakingHeartState(state::properties::CreakingHeartState::UPROOTED));
            defaultState = defaultState->setValue(*BlockStateProperties::NATURAL, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AXIS, BlockStateProperties::CREAKING_HEART_STATE,
                    BlockStateProperties::NATURAL);
    }
};

// Reference: PointedDripstoneBlock.java - VERTICAL_DIRECTION (default up),
// THICKNESS (default tip), WATERLOGGED (default false)
class PointedDripstoneBlockImpl : public Block {
public:
    explicit PointedDripstoneBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::VERTICAL_DIRECTION, core::Direction::UP);
            defaultState = defaultState->setValue(*BlockStateProperties::DRIPSTONE_THICKNESS,
                state::properties::DripstoneThickness(state::properties::DripstoneThickness::TIP));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::VERTICAL_DIRECTION, BlockStateProperties::DRIPSTONE_THICKNESS,
                    BlockStateProperties::WATERLOGGED);
    }
};

// Reference: MushroomBlock.java - canSurvive: below in mushroom_grow_block
// tag, OR light < 13 with solid-render below. During worldgen the sky-light
// engine has no data and returns 15 everywhere (SkyLightSectionStorage
// defaults), so the light branch NEVER passes: only the grow tag matters.
// Mushrooms are also NOT replaceable and NOT replaceable-by-trees in Java.
class MushroomBlockImpl : public BushBlock {
public:
    explicit MushroomBlockImpl(const Properties& properties) : BushBlock(properties) {}

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* below = level.getBlockState(pos.below());
        return ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
            below, "minecraft:mushroom_grow_block");
    }
};

// Reference: BeehiveBlock.java - HORIZONTAL_FACING (default north),
// LEVEL_HONEY (0-5, default 0)
class BeehiveBlockImpl : public Block {
public:
    explicit BeehiveBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::LEVEL_HONEY, 0);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::LEVEL_HONEY);
    }
};

// Reference: SculkCatalystBlock.java - BLOOM, default false
class SculkCatalystBlockImpl : public Block {
public:
    explicit SculkCatalystBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::BLOOM, false));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::BLOOM);
    }
};

// Reference: KelpBlock.java (GrowingPlantHeadBlock) - AGE (0-25), default 0.
// Worldgen only needs the property modeled; growth behavior is not simulated.
class Age25HeadBlock : public Block {
public:
    explicit Age25HeadBlock(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::AGE_25, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_25);
    }
};

// Reference: HugeMushroomBlock.java - six boolean face properties, all true by
// default. Huge-mushroom features set faces per cap position.
class MushroomCapBlockImpl : public Block {
public:
    explicit MushroomCapBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::UP, true);
            defaultState = defaultState->setValue(*BlockStateProperties::DOWN, true);
            defaultState = defaultState->setValue(*BlockStateProperties::NORTH, true);
            defaultState = defaultState->setValue(*BlockStateProperties::EAST, true);
            defaultState = defaultState->setValue(*BlockStateProperties::SOUTH, true);
            defaultState = defaultState->setValue(*BlockStateProperties::WEST, true);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::NORTH, BlockStateProperties::EAST,
                    BlockStateProperties::SOUTH, BlockStateProperties::WEST,
                    BlockStateProperties::UP, BlockStateProperties::DOWN);
    }
};


// Reference: BambooStalkBlock.java - AGE (0-1), LEAVES (none/small/large),
// STAGE (0-1); canSurvive = below in #minecraft:bamboo_plantable_on.
class BambooStalkBlockImpl : public Block {
public:
    explicit BambooStalkBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::AGE_1, 0);
            defaultState = defaultState->setValue(*BlockStateProperties::BAMBOO_LEAVES,
                state::properties::BambooLeaves(state::properties::BambooLeaves::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::STAGE, 0);
            registerDefaultState(defaultState);
        }
    }

    bool canSurvive(
        BlockState* /*state*/,
        const levelgen::WorldGenLevel& level,
        const core::BlockPos& pos
    ) const override {
        BlockState* below = level.getBlockState(pos.below());
        return below && minecraft::levelgen::blockpredicates::matchesBlockTagName(
            below, "minecraft:bamboo_plantable_on");
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_1, BlockStateProperties::BAMBOO_LEAVES,
                    BlockStateProperties::STAGE);
    }
};

// Reference: SnowLayerBlock.java - LAYERS (1-8), default 1
class SnowLayerBlockImpl : public Block {
public:
    explicit SnowLayerBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::LAYERS, 1));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LAYERS);
    }
};

} // namespace

// Static member definitions
bool minecraft::world::level::block::Blocks::s_initialized = false;
std::unordered_map<std::string, Block*> minecraft::world::level::block::Blocks::s_blocksByName;

// =========================================================================
// Basic blocks (no properties)
// =========================================================================
Block* minecraft::world::level::block::Blocks::AIR = nullptr;
Block* minecraft::world::level::block::Blocks::CAVE_AIR = nullptr;
Block* minecraft::world::level::block::Blocks::STONE = nullptr;
Block* minecraft::world::level::block::Blocks::GRANITE = nullptr;
Block* minecraft::world::level::block::Blocks::DIORITE = nullptr;
Block* minecraft::world::level::block::Blocks::ANDESITE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE = nullptr;
Block* minecraft::world::level::block::Blocks::COBBLESTONE = nullptr;
Block* minecraft::world::level::block::Blocks::MOSSY_COBBLESTONE = nullptr;
Block* minecraft::world::level::block::Blocks::DIRT = nullptr;
Block* minecraft::world::level::block::Blocks::ROOTED_DIRT = nullptr;
Block* minecraft::world::level::block::Blocks::COARSE_DIRT = nullptr;
Block* minecraft::world::level::block::Blocks::PODZOL = nullptr;
Block* minecraft::world::level::block::Blocks::GRASS_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::SAND = nullptr;
Block* minecraft::world::level::block::Blocks::GRAVEL = nullptr;
Block* minecraft::world::level::block::Blocks::BEDROCK = nullptr;
Block* minecraft::world::level::block::Blocks::WATER = nullptr;
Block* minecraft::world::level::block::Blocks::LAVA = nullptr;
Block* minecraft::world::level::block::Blocks::TUFF = nullptr;
Block* minecraft::world::level::block::Blocks::DRIPSTONE_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::POINTED_DRIPSTONE = nullptr;
Block* minecraft::world::level::block::Blocks::SANDSTONE = nullptr;

// Ice and snow
Block* minecraft::world::level::block::Blocks::SNOW_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::PACKED_ICE = nullptr;
Block* minecraft::world::level::block::Blocks::BLUE_ICE = nullptr;
Block* minecraft::world::level::block::Blocks::ICE = nullptr;
Block* minecraft::world::level::block::Blocks::POWDER_SNOW = nullptr;
Block* minecraft::world::level::block::Blocks::SNOW = nullptr;

// Geode blocks
Block* minecraft::world::level::block::Blocks::AMETHYST_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::BUDDING_AMETHYST = nullptr;
Block* minecraft::world::level::block::Blocks::CALCITE = nullptr;
Block* minecraft::world::level::block::Blocks::SMOOTH_BASALT = nullptr;
Block* minecraft::world::level::block::Blocks::SMALL_AMETHYST_BUD = nullptr;
Block* minecraft::world::level::block::Blocks::MEDIUM_AMETHYST_BUD = nullptr;
Block* minecraft::world::level::block::Blocks::LARGE_AMETHYST_BUD = nullptr;
Block* minecraft::world::level::block::Blocks::AMETHYST_CLUSTER = nullptr;

// Clay and mud blocks
Block* minecraft::world::level::block::Blocks::CLAY = nullptr;
Block* minecraft::world::level::block::Blocks::MUD = nullptr;
Block* minecraft::world::level::block::Blocks::MUDDY_MANGROVE_ROOTS = nullptr;
Block* minecraft::world::level::block::Blocks::MAGMA_BLOCK = nullptr;

// Sculk blocks
SculkBlock* minecraft::world::level::block::Blocks::SCULK = nullptr;
Block* minecraft::world::level::block::Blocks::SCULK_CATALYST = nullptr;
Block* minecraft::world::level::block::Blocks::SCULK_SENSOR = nullptr;
Block* minecraft::world::level::block::Blocks::SCULK_SHRIEKER = nullptr;
SculkVeinBlock* minecraft::world::level::block::Blocks::SCULK_VEIN = nullptr;

// Ore blocks
Block* minecraft::world::level::block::Blocks::COPPER_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_COPPER_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::IRON_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_IRON_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::COAL_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_COAL_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::GOLD_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_GOLD_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DIAMOND_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_DIAMOND_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::REDSTONE_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_REDSTONE_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::LAPIS_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_LAPIS_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::EMERALD_ORE = nullptr;
Block* minecraft::world::level::block::Blocks::DEEPSLATE_EMERALD_ORE = nullptr;

// Raw ore blocks
Block* minecraft::world::level::block::Blocks::RAW_COPPER_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::RAW_IRON_BLOCK = nullptr;

// Infested blocks
Block* minecraft::world::level::block::Blocks::INFESTED_STONE = nullptr;
Block* minecraft::world::level::block::Blocks::INFESTED_DEEPSLATE = nullptr;

// Terracotta
Block* minecraft::world::level::block::Blocks::TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::WHITE_TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::ORANGE_TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::YELLOW_TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::BROWN_TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::RED_TERRACOTTA = nullptr;
Block* minecraft::world::level::block::Blocks::LIGHT_GRAY_TERRACOTTA = nullptr;

// Vegetation - small plants
BushBlock* minecraft::world::level::block::Blocks::SHORT_GRASS = nullptr;
DoublePlantBlock* minecraft::world::level::block::Blocks::TALL_GRASS = nullptr;
BushBlock* minecraft::world::level::block::Blocks::FERN = nullptr;
DoublePlantBlock* minecraft::world::level::block::Blocks::LARGE_FERN = nullptr;
BushBlock* minecraft::world::level::block::Blocks::DEAD_BUSH = nullptr;
Block* minecraft::world::level::block::Blocks::SHORT_DRY_GRASS = nullptr;
Block* minecraft::world::level::block::Blocks::TALL_DRY_GRASS = nullptr;
BushBlock* minecraft::world::level::block::Blocks::BUSH = nullptr;

// Flowers
BushBlock* minecraft::world::level::block::Blocks::DANDELION = nullptr;
BushBlock* minecraft::world::level::block::Blocks::POPPY = nullptr;
BushBlock* minecraft::world::level::block::Blocks::BLUE_ORCHID = nullptr;
BushBlock* minecraft::world::level::block::Blocks::ALLIUM = nullptr;
BushBlock* minecraft::world::level::block::Blocks::AZURE_BLUET = nullptr;
BushBlock* minecraft::world::level::block::Blocks::RED_TULIP = nullptr;
BushBlock* minecraft::world::level::block::Blocks::ORANGE_TULIP = nullptr;
BushBlock* minecraft::world::level::block::Blocks::WHITE_TULIP = nullptr;
BushBlock* minecraft::world::level::block::Blocks::PINK_TULIP = nullptr;
BushBlock* minecraft::world::level::block::Blocks::OXEYE_DAISY = nullptr;
BushBlock* minecraft::world::level::block::Blocks::CORNFLOWER = nullptr;
BushBlock* minecraft::world::level::block::Blocks::LILY_OF_THE_VALLEY = nullptr;

// Tall flowers
TallFlowerBlock* minecraft::world::level::block::Blocks::SUNFLOWER = nullptr;
TallFlowerBlock* minecraft::world::level::block::Blocks::LILAC = nullptr;
TallFlowerBlock* minecraft::world::level::block::Blocks::ROSE_BUSH = nullptr;
TallFlowerBlock* minecraft::world::level::block::Blocks::PEONY = nullptr;

// Mushrooms
BushBlock* minecraft::world::level::block::Blocks::BROWN_MUSHROOM = nullptr;
BushBlock* minecraft::world::level::block::Blocks::RED_MUSHROOM = nullptr;

// Huge mushroom blocks
Block* minecraft::world::level::block::Blocks::BROWN_MUSHROOM_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::RED_MUSHROOM_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::MUSHROOM_STEM = nullptr;

// Leaf litter and vines
LeafLitterBlock* minecraft::world::level::block::Blocks::LEAF_LITTER = nullptr;
FlowerBedBlock* minecraft::world::level::block::Blocks::PINK_PETALS = nullptr;
FlowerBedBlock* minecraft::world::level::block::Blocks::WILDFLOWERS = nullptr;
Block* minecraft::world::level::block::Blocks::VINE = nullptr;

// Moss and lush cave vegetation
Block* minecraft::world::level::block::Blocks::MOSS_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::MOSS_CARPET = nullptr;
Block* minecraft::world::level::block::Blocks::CAVE_VINES = nullptr;
Block* minecraft::world::level::block::Blocks::CAVE_VINES_PLANT = nullptr;
GlowLichenBlock* minecraft::world::level::block::Blocks::GLOW_LICHEN = nullptr;
Block* minecraft::world::level::block::Blocks::AZALEA = nullptr;
Block* minecraft::world::level::block::Blocks::FLOWERING_AZALEA = nullptr;
HangingRootsBlock* minecraft::world::level::block::Blocks::HANGING_ROOTS = nullptr;
SporeBlossomBlock* minecraft::world::level::block::Blocks::SPORE_BLOSSOM = nullptr;
Block* minecraft::world::level::block::Blocks::BIG_DRIPLEAF = nullptr;
Block* minecraft::world::level::block::Blocks::BIG_DRIPLEAF_STEM = nullptr;
Block* minecraft::world::level::block::Blocks::SMALL_DRIPLEAF = nullptr;

// Pale garden vegetation
Block* minecraft::world::level::block::Blocks::PALE_MOSS_BLOCK = nullptr;
Block* minecraft::world::level::block::Blocks::PALE_MOSS_CARPET = nullptr;
Block* minecraft::world::level::block::Blocks::PALE_HANGING_MOSS = nullptr;
EyeblossomBlock* minecraft::world::level::block::Blocks::CLOSED_EYEBLOSSOM = nullptr;

// Ocean vegetation
Block* minecraft::world::level::block::Blocks::SEAGRASS = nullptr;
Block* minecraft::world::level::block::Blocks::TALL_SEAGRASS = nullptr;
Block* minecraft::world::level::block::Blocks::KELP = nullptr;
Block* minecraft::world::level::block::Blocks::KELP_PLANT = nullptr;
Block* minecraft::world::level::block::Blocks::BAMBOO = nullptr;

// Other vegetation
Block* minecraft::world::level::block::Blocks::CACTUS = nullptr;
Block* minecraft::world::level::block::Blocks::CACTUS_FLOWER = nullptr;
Block* minecraft::world::level::block::Blocks::SUGAR_CANE = nullptr;
Block* minecraft::world::level::block::Blocks::SWEET_BERRY_BUSH = nullptr;
Block* minecraft::world::level::block::Blocks::LILY_PAD = nullptr;
Block* minecraft::world::level::block::Blocks::FIREFLY_BUSH = nullptr;
Block* minecraft::world::level::block::Blocks::PUMPKIN = nullptr;
Block* minecraft::world::level::block::Blocks::MELON = nullptr;
Block* minecraft::world::level::block::Blocks::COCOA = nullptr;
Block* minecraft::world::level::block::Blocks::MANGROVE_ROOTS = nullptr;
Block* minecraft::world::level::block::Blocks::OAK_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::SPRUCE_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::BIRCH_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::JUNGLE_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::ACACIA_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::CHERRY_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::DARK_OAK_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::PALE_OAK_SAPLING = nullptr;
Block* minecraft::world::level::block::Blocks::MANGROVE_PROPAGULE = nullptr;

// Dungeon blocks
Block* minecraft::world::level::block::Blocks::SPAWNER = nullptr;
Block* minecraft::world::level::block::Blocks::CHEST = nullptr;
Block* minecraft::world::level::block::Blocks::BEE_NEST = nullptr;

// =========================================================================
// Blocks with properties
// =========================================================================
StairBlock* minecraft::world::level::block::Blocks::OAK_STAIRS = nullptr;
StairBlock* minecraft::world::level::block::Blocks::STONE_STAIRS = nullptr;
StairBlock* minecraft::world::level::block::Blocks::COBBLESTONE_STAIRS = nullptr;

SlabBlock* minecraft::world::level::block::Blocks::OAK_SLAB = nullptr;
SlabBlock* minecraft::world::level::block::Blocks::STONE_SLAB = nullptr;
SlabBlock* minecraft::world::level::block::Blocks::COBBLESTONE_SLAB = nullptr;

FenceBlock* minecraft::world::level::block::Blocks::OAK_FENCE = nullptr;
FenceBlock* minecraft::world::level::block::Blocks::NETHER_BRICK_FENCE = nullptr;

DoorBlock* minecraft::world::level::block::Blocks::OAK_DOOR = nullptr;
DoorBlock* minecraft::world::level::block::Blocks::IRON_DOOR = nullptr;

WallBlock* minecraft::world::level::block::Blocks::COBBLESTONE_WALL = nullptr;
WallBlock* minecraft::world::level::block::Blocks::STONE_BRICK_WALL = nullptr;

// Leaves
LeavesBlock* minecraft::world::level::block::Blocks::OAK_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::SPRUCE_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::BIRCH_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::JUNGLE_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::ACACIA_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::DARK_OAK_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::AZALEA_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::FLOWERING_AZALEA_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::MANGROVE_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::CHERRY_LEAVES = nullptr;
LeavesBlock* minecraft::world::level::block::Blocks::PALE_OAK_LEAVES = nullptr;

// Logs
RotatedPillarBlock* minecraft::world::level::block::Blocks::OAK_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::SPRUCE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::BIRCH_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::JUNGLE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::ACACIA_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::DARK_OAK_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::MANGROVE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::CHERRY_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::PALE_OAK_LOG = nullptr;

// Stripped logs
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_OAK_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_SPRUCE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_BIRCH_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_JUNGLE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_ACACIA_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_DARK_OAK_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_MANGROVE_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_CHERRY_LOG = nullptr;
RotatedPillarBlock* minecraft::world::level::block::Blocks::STRIPPED_PALE_OAK_LOG = nullptr;

// =========================================================================
// Helper methods
// =========================================================================

void minecraft::world::level::block::Blocks::registerBlock(const std::string& name, Block* block) {
    // Identity interning relies on one Block instance per name: pointer
    // compares replace string compares in hot paths, so a silent overwrite
    // here would leave stale instances whose states break pointer identity.
    auto [it, inserted] = s_blocksByName.emplace(name, block);
    if (!inserted && it->second != block) {
        throw std::runtime_error("Blocks::registerBlock: duplicate registration of " + name);
    }
}

Block* minecraft::world::level::block::Blocks::createSimpleBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createNoOcclusionBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noOcclusion();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createNoCollisionBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createForceSolidOnNoCollisionBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).forceSolidOn().noCollission();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createForceSolidOnNoOcclusionBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).forceSolidOn().noOcclusion();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createAirBlock(const std::string& name) {
    Block::Properties props;
    // Java AirBlock properties include .replaceable() - canBeReplaced() is
    // true for air (MossyCarpetBlock topper precondition relies on this).
    props.setId(name).air().replaceable();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createLiquidBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).liquid().noCollission().replaceable();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createPlantBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createReplaceablePlantBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

CarpetBlock* minecraft::world::level::block::Blocks::createCarpetBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new CarpetBlock(props);
    registerBlock(name, block);
    return block;
}

MossyCarpetBlock* minecraft::world::level::block::Blocks::createMossyCarpetBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission().noOcclusion();
    auto block = new MossyCarpetBlock(props);
    registerBlock(name, block);
    return block;
}

BushBlock* minecraft::world::level::block::Blocks::createBushBlock(const std::string& name, bool replaceable) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    // Java: only the "replaceable plants" (short_grass, fern, bush) carry
    // BlockBehaviour.Properties.replaceable(); flowers and saplings do NOT.
    if (replaceable) {
        props.replaceable();
    }
    auto block = new BushBlock(props);
    registerBlock(name, block);
    return block;
}

BushBlock* minecraft::world::level::block::Blocks::createDryVegetationBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new DryVegetationBlock(props);
    registerBlock(name, block);
    return block;
}

BushBlock* minecraft::world::level::block::Blocks::createCactusFlowerBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new CactusFlowerBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createSugarCaneBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new SugarCaneBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createSweetBerryBushBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new SweetBerryBushBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createWaterlilyBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new WaterlilyBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createFireflyBushBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new FireflyBushBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createPaleHangingMossBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new PaleHangingMossBlockImpl(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createCocoaBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new CocoaBlockImpl(props);
    registerBlock(name, block);
    return block;
}

VineBlock* minecraft::world::level::block::Blocks::createVineBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable();
    auto block = new VineBlock(props);
    registerBlock(name, block);
    return block;
}

AzaleaBlock* minecraft::world::level::block::Blocks::createAzaleaBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).forceSolidOff().noOcclusion();
    auto block = new AzaleaBlock(props);
    registerBlock(name, block);
    return block;
}

CaveVinesBlock* minecraft::world::level::block::Blocks::createCaveVinesBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new CaveVinesBlock(props);
    registerBlock(name, block);
    return block;
}

CaveVinesPlantBlock* minecraft::world::level::block::Blocks::createCaveVinesPlantBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new CaveVinesPlantBlock(props);
    registerBlock(name, block);
    return block;
}

GlowLichenBlock* minecraft::world::level::block::Blocks::createGlowLichenBlock(const std::string& name) {
    // Java: BlockBehaviour.Properties.of().replaceable().noCollission()...
    Block::Properties props;
    props.setId(name).noCollission().replaceable();
    auto block = new GlowLichenBlock(props);
    registerBlock(name, block);
    return block;
}

HangingRootsBlock* minecraft::world::level::block::Blocks::createHangingRootsBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission().replaceable();
    auto block = new HangingRootsBlock(props);
    registerBlock(name, block);
    return block;
}

SporeBlossomBlock* minecraft::world::level::block::Blocks::createSporeBlossomBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new SporeBlossomBlock(props);
    registerBlock(name, block);
    return block;
}

SmallDripleafBlock* minecraft::world::level::block::Blocks::createSmallDripleafBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new SmallDripleafBlock(props);
    registerBlock(name, block);
    return block;
}

BigDripleafBlock* minecraft::world::level::block::Blocks::createBigDripleafBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).forceSolidOff();
    auto block = new BigDripleafBlock(props);
    registerBlock(name, block);
    return block;
}

BigDripleafStemBlock* minecraft::world::level::block::Blocks::createBigDripleafStemBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission();
    auto block = new BigDripleafStemBlock(props);
    registerBlock(name, block);
    return block;
}

DoublePlantBlock* minecraft::world::level::block::Blocks::createDoublePlantBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new DoublePlantBlock(props);
    registerBlock(name, block);
    return block;
}

TallFlowerBlock* minecraft::world::level::block::Blocks::createTallFlowerBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new TallFlowerBlock(props);
    registerBlock(name, block);
    return block;
}

FlowerBedBlock* minecraft::world::level::block::Blocks::createFlowerBedBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new FlowerBedBlock(props);
    registerBlock(name, block);
    return block;
}

EyeblossomBlock* minecraft::world::level::block::Blocks::createEyeblossomBlock(const std::string& name, bool open) {
    Block::Properties props;
    props.setId(name).replaceableByTrees();
    auto block = new EyeblossomBlock(open, props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createReplaceableByTreesBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).noCollission().replaceable().replaceableByTrees();
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

LeavesBlock* minecraft::world::level::block::Blocks::createLeavesBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).leaves().noOcclusion().replaceableByTrees();
    auto block = new LeavesBlock(props);
    registerBlock(name, block);
    return block;
}

RotatedPillarBlock* minecraft::world::level::block::Blocks::createLogBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).log();
    auto block = new RotatedPillarBlock(props);
    registerBlock(name, block);
    return block;
}

// =========================================================================
// Bootstrap
// =========================================================================

void minecraft::world::level::block::Blocks::bootstrap() {
    if (s_initialized) return;

    // Initialize BlockStateProperties first
    state::properties::BlockStateProperties::initialize();

    // =========================================================================
    // Air blocks
    // =========================================================================
    AIR = createAirBlock("minecraft:air");
    CAVE_AIR = createAirBlock("minecraft:cave_air");

    // =========================================================================
    // Basic terrain blocks
    // =========================================================================
    STONE = createSimpleBlock("minecraft:stone");
    GRANITE = createSimpleBlock("minecraft:granite");
    DIORITE = createSimpleBlock("minecraft:diorite");
    ANDESITE = createSimpleBlock("minecraft:andesite");
    {
        // Reference: Blocks.java - deepslate is a RotatedPillarBlock (axis=y default)
        Block::Properties props;
        props.setId("minecraft:deepslate");
        DEEPSLATE = new RotatedPillarBlock(props);
        registerBlock("minecraft:deepslate", DEEPSLATE);
    }
    COBBLESTONE = createSimpleBlock("minecraft:cobblestone");
    MOSSY_COBBLESTONE = createSimpleBlock("minecraft:mossy_cobblestone");
    DIRT = createSimpleBlock("minecraft:dirt");
    ROOTED_DIRT = createSimpleBlock("minecraft:rooted_dirt");
    COARSE_DIRT = createSimpleBlock("minecraft:coarse_dirt");
    {
        // Reference: Blocks.java - SnowyDirtBlock (snowy=false default)
        Block::Properties props;
        props.setId("minecraft:podzol");
        PODZOL = new SnowyDirtBlockImpl(props);
        registerBlock("minecraft:podzol", PODZOL);
    }
    {
        Block::Properties props;
        props.setId("minecraft:grass_block");
        GRASS_BLOCK = new SnowyDirtBlockImpl(props);
        registerBlock("minecraft:grass_block", GRASS_BLOCK);
    }
    SAND = createSimpleBlock("minecraft:sand");
    createSimpleBlock("minecraft:red_sand");
    createSimpleBlock("minecraft:red_sandstone");
    {
        // Reference: Blocks.java - MYCELIUM is a SnowyDirtBlock (snowy=false)
        Block::Properties props;
        props.setId("minecraft:mycelium");
        registerBlock("minecraft:mycelium", new SnowyDirtBlockImpl(props));
    }
    // Registered so surface rules never resolve to null states; Nether/End
    // surfaces themselves are out of scope.
    createSimpleBlock("minecraft:basalt");
    createSimpleBlock("minecraft:blackstone");
    createSimpleBlock("minecraft:crimson_nylium");
    createSimpleBlock("minecraft:end_stone");
    createSimpleBlock("minecraft:nether_wart_block");
    createSimpleBlock("minecraft:netherrack");
    createSimpleBlock("minecraft:soul_sand");
    createSimpleBlock("minecraft:soul_soil");
    createSimpleBlock("minecraft:warped_nylium");
    createSimpleBlock("minecraft:warped_wart_block");
    GRAVEL = createSimpleBlock("minecraft:gravel");
    BEDROCK = createSimpleBlock("minecraft:bedrock");
    TUFF = createSimpleBlock("minecraft:tuff");
    DRIPSTONE_BLOCK = createSimpleBlock("minecraft:dripstone_block");
    {
        Block::Properties props;
        // Java: noOcclusion + dynamicShape - never solid-render.
        props.setId("minecraft:pointed_dripstone").noOcclusion();
        POINTED_DRIPSTONE = new PointedDripstoneBlockImpl(props);
        registerBlock("minecraft:pointed_dripstone", POINTED_DRIPSTONE);
    }
    SANDSTONE = createSimpleBlock("minecraft:sandstone");

    // =========================================================================
    // Liquids
    // =========================================================================
    {
        Block::Properties props;
        props.setId("minecraft:water").liquid().noCollission().replaceable().replaceableByTrees();
        WATER = new WorldgenLiquidBlock(props);
        registerBlock("minecraft:water", WATER);
    }
    {
        Block::Properties props;
        props.setId("minecraft:lava").liquid().noCollission().replaceable();
        LAVA = new WorldgenLiquidBlock(props);
        registerBlock("minecraft:lava", LAVA);
    }

    // =========================================================================
    // Ice and snow
    // =========================================================================
    SNOW_BLOCK = createSimpleBlock("minecraft:snow_block");
    PACKED_ICE = createSimpleBlock("minecraft:packed_ice");
    BLUE_ICE = createSimpleBlock("minecraft:blue_ice");
    // Java: ice has noOcclusion() - it is NOT solid-render (matters for e.g.
    // PlaceOnGroundDecorator's ground check).
    {
        Block::Properties props;
        props.setId("minecraft:ice").noOcclusion();
        ICE = new Block(props);
        registerBlock("minecraft:ice", ICE);
    }
    {
        Block::Properties props;
        props.setId("minecraft:powder_snow").noCollission();
        POWDER_SNOW = new Block(props);
        registerBlock("minecraft:powder_snow", POWDER_SNOW);
    }
    // Snow layer block (1-8 layers, default 1)
    {
        Block::Properties props;
        props.setId("minecraft:snow").noCollission().replaceable();
        SNOW = new SnowLayerBlockImpl(props);
        registerBlock("minecraft:snow", SNOW);
    }

    // =========================================================================
    // Geode blocks
    // Reference: Used in amethyst geode feature
    // =========================================================================
    AMETHYST_BLOCK = createSimpleBlock("minecraft:amethyst_block");
    {
        Block::Properties props;
        props.setId("minecraft:budding_amethyst");
        BUDDING_AMETHYST = new BuddingAmethystBlock(props);
        registerBlock("minecraft:budding_amethyst", BUDDING_AMETHYST);
    }
    CALCITE = createSimpleBlock("minecraft:calcite");
    SMOOTH_BASALT = createSimpleBlock("minecraft:smooth_basalt");
    {
        Block::Properties props;
        props.setId("minecraft:small_amethyst_bud").forceSolidOn().noOcclusion();
        SMALL_AMETHYST_BUD = new AmethystClusterBlock(props);
        registerBlock("minecraft:small_amethyst_bud", SMALL_AMETHYST_BUD);
    }
    {
        Block::Properties props;
        props.setId("minecraft:medium_amethyst_bud").forceSolidOn().noOcclusion();
        MEDIUM_AMETHYST_BUD = new AmethystClusterBlock(props);
        registerBlock("minecraft:medium_amethyst_bud", MEDIUM_AMETHYST_BUD);
    }
    {
        Block::Properties props;
        props.setId("minecraft:large_amethyst_bud").forceSolidOn().noOcclusion();
        LARGE_AMETHYST_BUD = new AmethystClusterBlock(props);
        registerBlock("minecraft:large_amethyst_bud", LARGE_AMETHYST_BUD);
    }
    {
        Block::Properties props;
        props.setId("minecraft:amethyst_cluster").forceSolidOn().noOcclusion();
        AMETHYST_CLUSTER = new AmethystClusterBlock(props);
        registerBlock("minecraft:amethyst_cluster", AMETHYST_CLUSTER);
    }

    // =========================================================================
    // Clay and mud blocks
    // =========================================================================
    CLAY = createSimpleBlock("minecraft:clay");
    MUD = createSimpleBlock("minecraft:mud");
    {
        // Reference: Blocks.java BONE_BLOCK - RotatedPillarBlock (axis=y).
        Block::Properties boneProps;
        boneProps.setId("minecraft:bone_block");
        registerBlock("minecraft:bone_block", new RotatedPillarBlock(boneProps));
    }
    {
        // Reference: Blocks.java MUDDY_MANGROVE_ROOTS - RotatedPillarBlock (axis=y).
        Block::Properties muddyProps;
        muddyProps.setId("minecraft:muddy_mangrove_roots");
        MUDDY_MANGROVE_ROOTS = new RotatedPillarBlock(muddyProps);
        registerBlock("minecraft:muddy_mangrove_roots", MUDDY_MANGROVE_ROOTS);
    }
    MAGMA_BLOCK = createSimpleBlock("minecraft:magma_block");

    // =========================================================================
    // Sculk blocks
    // =========================================================================
    {
        Block::Properties props;
        props.setId("minecraft:sculk");
        SCULK = new SculkBlock(props);
        registerBlock("minecraft:sculk", SCULK);
    }
    {
        Block::Properties props;
        props.setId("minecraft:sculk_catalyst");
        SCULK_CATALYST = new SculkCatalystBlockImpl(props);
        registerBlock("minecraft:sculk_catalyst", SCULK_CATALYST);
    }
    {
        Block::Properties props;
        props.setId("minecraft:sculk_sensor");
        SCULK_SENSOR = new SculkSensorBlock(props);
        registerBlock("minecraft:sculk_sensor", SCULK_SENSOR);
    }
    {
        Block::Properties props;
        props.setId("minecraft:sculk_shrieker");
        SCULK_SHRIEKER = new SculkShriekerBlock(props);
        registerBlock("minecraft:sculk_shrieker", SCULK_SHRIEKER);
    }

    // SculkVeinBlock with proper multiface properties (6 face directions + waterlogged)
    // Reference: MultifaceBlock.java - each face can be independently enabled
    {
        Block::Properties props;
        props.setId("minecraft:sculk_vein");
        props.forceSolidOn();
        props.noCollission();
        SCULK_VEIN = new SculkVeinBlock(props);
        registerBlock("minecraft:sculk_vein", SCULK_VEIN);
    }

    // =========================================================================
    // Ore blocks
    // =========================================================================
    COPPER_ORE = createSimpleBlock("minecraft:copper_ore");
    DEEPSLATE_COPPER_ORE = createSimpleBlock("minecraft:deepslate_copper_ore");
    IRON_ORE = createSimpleBlock("minecraft:iron_ore");
    DEEPSLATE_IRON_ORE = createSimpleBlock("minecraft:deepslate_iron_ore");
    COAL_ORE = createSimpleBlock("minecraft:coal_ore");
    DEEPSLATE_COAL_ORE = createSimpleBlock("minecraft:deepslate_coal_ore");
    GOLD_ORE = createSimpleBlock("minecraft:gold_ore");
    DEEPSLATE_GOLD_ORE = createSimpleBlock("minecraft:deepslate_gold_ore");
    DIAMOND_ORE = createSimpleBlock("minecraft:diamond_ore");
    DEEPSLATE_DIAMOND_ORE = createSimpleBlock("minecraft:deepslate_diamond_ore");
    {
        // Reference: Blocks.java - RedStoneOreBlock (lit=false default)
        Block::Properties props;
        props.setId("minecraft:redstone_ore");
        REDSTONE_ORE = new RedStoneOreBlockImpl(props);
        registerBlock("minecraft:redstone_ore", REDSTONE_ORE);
    }
    {
        Block::Properties props;
        props.setId("minecraft:deepslate_redstone_ore");
        DEEPSLATE_REDSTONE_ORE = new RedStoneOreBlockImpl(props);
        registerBlock("minecraft:deepslate_redstone_ore", DEEPSLATE_REDSTONE_ORE);
    }
    LAPIS_ORE = createSimpleBlock("minecraft:lapis_ore");
    DEEPSLATE_LAPIS_ORE = createSimpleBlock("minecraft:deepslate_lapis_ore");
    EMERALD_ORE = createSimpleBlock("minecraft:emerald_ore");
    DEEPSLATE_EMERALD_ORE = createSimpleBlock("minecraft:deepslate_emerald_ore");

    // =========================================================================
    // Raw ore blocks
    // =========================================================================
    RAW_COPPER_BLOCK = createSimpleBlock("minecraft:raw_copper_block");
    RAW_IRON_BLOCK = createSimpleBlock("minecraft:raw_iron_block");

    // =========================================================================
    // Infested blocks
    // Reference: Used by silverfish spawning and ore infested feature
    // =========================================================================
    INFESTED_STONE = createSimpleBlock("minecraft:infested_stone");
    {
        // Reference: Blocks.java - RotatedPillarInfestedBlock (axis=y default)
        Block::Properties props;
        props.setId("minecraft:infested_deepslate");
        INFESTED_DEEPSLATE = new RotatedPillarBlock(props);
        registerBlock("minecraft:infested_deepslate", INFESTED_DEEPSLATE);
    }

    // =========================================================================
    // Terracotta blocks
    // =========================================================================
    TERRACOTTA = createSimpleBlock("minecraft:terracotta");
    WHITE_TERRACOTTA = createSimpleBlock("minecraft:white_terracotta");
    ORANGE_TERRACOTTA = createSimpleBlock("minecraft:orange_terracotta");
    YELLOW_TERRACOTTA = createSimpleBlock("minecraft:yellow_terracotta");
    BROWN_TERRACOTTA = createSimpleBlock("minecraft:brown_terracotta");
    RED_TERRACOTTA = createSimpleBlock("minecraft:red_terracotta");
    LIGHT_GRAY_TERRACOTTA = createSimpleBlock("minecraft:light_gray_terracotta");

    // =========================================================================
    // Vegetation - small plants (no collision)
    // =========================================================================
    SHORT_GRASS = createBushBlock("minecraft:short_grass");
    TALL_GRASS = createDoublePlantBlock("minecraft:tall_grass");
    FERN = createBushBlock("minecraft:fern");
    LARGE_FERN = createDoublePlantBlock("minecraft:large_fern");
    DEAD_BUSH = createDryVegetationBlock("minecraft:dead_bush");
    SHORT_DRY_GRASS = createDryVegetationBlock("minecraft:short_dry_grass");
    TALL_DRY_GRASS = createDryVegetationBlock("minecraft:tall_dry_grass");
    BUSH = createBushBlock("minecraft:bush");

    // =========================================================================
    // Flowers (no collision)
    // =========================================================================
    DANDELION = createBushBlock("minecraft:dandelion", false);
    POPPY = createBushBlock("minecraft:poppy", false);
    BLUE_ORCHID = createBushBlock("minecraft:blue_orchid", false);
    ALLIUM = createBushBlock("minecraft:allium", false);
    AZURE_BLUET = createBushBlock("minecraft:azure_bluet", false);
    RED_TULIP = createBushBlock("minecraft:red_tulip", false);
    ORANGE_TULIP = createBushBlock("minecraft:orange_tulip", false);
    WHITE_TULIP = createBushBlock("minecraft:white_tulip", false);
    PINK_TULIP = createBushBlock("minecraft:pink_tulip", false);
    OXEYE_DAISY = createBushBlock("minecraft:oxeye_daisy", false);
    CORNFLOWER = createBushBlock("minecraft:cornflower", false);
    LILY_OF_THE_VALLEY = createBushBlock("minecraft:lily_of_the_valley", false);

    // =========================================================================
    // Tall flowers (two-block, no collision)
    // =========================================================================
    SUNFLOWER = createTallFlowerBlock("minecraft:sunflower");
    LILAC = createTallFlowerBlock("minecraft:lilac");
    ROSE_BUSH = createTallFlowerBlock("minecraft:rose_bush");
    PEONY = createTallFlowerBlock("minecraft:peony");

    // =========================================================================
    // Mushrooms (small, no collision)
    // =========================================================================
    {
        Block::Properties props;
        props.setId("minecraft:brown_mushroom");
        BROWN_MUSHROOM = new MushroomBlockImpl(props);
        registerBlock("minecraft:brown_mushroom", BROWN_MUSHROOM);
    }
    {
        Block::Properties props;
        props.setId("minecraft:red_mushroom");
        RED_MUSHROOM = new MushroomBlockImpl(props);
        registerBlock("minecraft:red_mushroom", RED_MUSHROOM);
    }

    // =========================================================================
    // Huge mushroom blocks (solid)
    // Note: These have directional properties in full implementation
    // =========================================================================
    {
        // Reference: Blocks.java - HugeMushroomBlock (6 face booleans, all true)
        Block::Properties props;
        props.setId("minecraft:brown_mushroom_block");
        BROWN_MUSHROOM_BLOCK = new MushroomCapBlockImpl(props);
        registerBlock("minecraft:brown_mushroom_block", BROWN_MUSHROOM_BLOCK);
    }
    {
        Block::Properties props;
        props.setId("minecraft:red_mushroom_block");
        RED_MUSHROOM_BLOCK = new MushroomCapBlockImpl(props);
        registerBlock("minecraft:red_mushroom_block", RED_MUSHROOM_BLOCK);
    }
    {
        // Reference: Blocks.java - mushroom_stem is also a HugeMushroomBlock
        Block::Properties props;
        props.setId("minecraft:mushroom_stem");
        MUSHROOM_STEM = new MushroomCapBlockImpl(props);
        registerBlock("minecraft:mushroom_stem", MUSHROOM_STEM);
    }

    // =========================================================================
    // Leaf litter and vines
    // =========================================================================
    // LeafLitterBlock with proper properties (HORIZONTAL_FACING, SEGMENT_AMOUNT)
    // Reference: LeafLitterBlock.java
    {
        Block::Properties props;
        props.noCollission().replaceable().setId("minecraft:leaf_litter").replaceableByTrees();
        LEAF_LITTER = new LeafLitterBlock(props);
        registerBlock("minecraft:leaf_litter", LEAF_LITTER);
    }
    PINK_PETALS = createFlowerBedBlock("minecraft:pink_petals");
    WILDFLOWERS = createFlowerBedBlock("minecraft:wildflowers");
    VINE = createVineBlock("minecraft:vine");

    // =========================================================================
    // Moss and lush cave vegetation
    // Reference: Used in lush caves biome features
    // =========================================================================
    MOSS_BLOCK = createSimpleBlock("minecraft:moss_block");
    MOSS_CARPET = createCarpetBlock("minecraft:moss_carpet");
    CAVE_VINES = createCaveVinesBlock("minecraft:cave_vines");
    CAVE_VINES_PLANT = createCaveVinesPlantBlock("minecraft:cave_vines_plant");
    GLOW_LICHEN = createGlowLichenBlock("minecraft:glow_lichen");
    AZALEA = createAzaleaBlock("minecraft:azalea");
    FLOWERING_AZALEA = createAzaleaBlock("minecraft:flowering_azalea");
    HANGING_ROOTS = createHangingRootsBlock("minecraft:hanging_roots");
    SPORE_BLOSSOM = createSporeBlossomBlock("minecraft:spore_blossom");
    BIG_DRIPLEAF = createBigDripleafBlock("minecraft:big_dripleaf");
    BIG_DRIPLEAF_STEM = createBigDripleafStemBlock("minecraft:big_dripleaf_stem");
    SMALL_DRIPLEAF = createSmallDripleafBlock("minecraft:small_dripleaf");

    // =========================================================================
    // Pale garden vegetation
    // Reference: Used in pale garden biome features
    // =========================================================================
    PALE_MOSS_BLOCK = createSimpleBlock("minecraft:pale_moss_block");
    {
        Block::Properties props;
        props.setId("minecraft:creaking_heart");
        registerBlock("minecraft:creaking_heart", new CreakingHeartBlockImpl(props));
    }
    PALE_MOSS_CARPET = createMossyCarpetBlock("minecraft:pale_moss_carpet");
    PALE_HANGING_MOSS = createPaleHangingMossBlock("minecraft:pale_hanging_moss");
    CLOSED_EYEBLOSSOM = createEyeblossomBlock("minecraft:closed_eyeblossom", false);

    // =========================================================================
    // Ocean vegetation
    // Reference: Used in ocean biome features
    // =========================================================================
    SEAGRASS = createReplaceableByTreesBlock("minecraft:seagrass");
    {
        // Reference: Blocks.java - TallSeagrassBlock extends DoublePlantBlock (HALF property)
        Block::Properties props;
        props.setId("minecraft:tall_seagrass").noCollission().replaceable().replaceableByTrees();
        TALL_SEAGRASS = new DoublePlantBlock(props);
        registerBlock("minecraft:tall_seagrass", TALL_SEAGRASS);
    }
    {
        // Reference: Blocks.java - KelpBlock has AGE (0-25)
        Block::Properties props;
        props.setId("minecraft:kelp").noCollission();
        KELP = new Age25HeadBlock(props);
        registerBlock("minecraft:kelp", KELP);
    }
    KELP_PLANT = createReplaceablePlantBlock("minecraft:kelp_plant");
    {
        // Reference: Blocks.java bamboo - forceSolidOn() => blocksMotion TRUE
        // (updates live OCEAN_FLOOR/MOTION_BLOCKING heightmaps), but its
        // dynamic thin shape is never face-full/sturdy (vines can't attach).
        Block::Properties props;
        props.setId("minecraft:bamboo");
        BAMBOO = new BambooStalkBlockImpl(props);
        registerBlock("minecraft:bamboo", BAMBOO);
    }

    // =========================================================================
    // Other vegetation
    // =========================================================================
    {
        Block::Properties cactusProps;
        cactusProps.setId("minecraft:cactus");
        CACTUS = new CactusBlockImpl(cactusProps);
        registerBlock("minecraft:cactus", CACTUS);
    }
    {
        // Live corals (Blocks.java): coral blocks are plain solids; plants,
        // fans and wall fans are noCollission; sea pickle keeps collision but
        // is noOcclusion. Dead variants are not needed for worldgen.
        static const char* kCoralTypes[] = {"tube", "brain", "bubble", "fire", "horn"};
        for (const char* type : kCoralTypes) {
            createSimpleBlock("minecraft:" + std::string(type) + "_coral_block");

            Block::Properties plantProps;
            plantProps.setId("minecraft:" + std::string(type) + "_coral").noCollission();
            registerBlock(plantProps.getIdentifier(), new WaterloggedDefaultTrueBlockImpl(plantProps));

            Block::Properties fanProps;
            fanProps.setId("minecraft:" + std::string(type) + "_coral_fan").noCollission();
            registerBlock(fanProps.getIdentifier(), new WaterloggedDefaultTrueBlockImpl(fanProps));

            Block::Properties wallFanProps;
            wallFanProps.setId("minecraft:" + std::string(type) + "_coral_wall_fan").noCollission();
            registerBlock(wallFanProps.getIdentifier(), new CoralWallFanBlockImpl(wallFanProps));
        }

        Block::Properties pickleProps;
        // noCollission here models Java's legacySolid=false (PLANT-like): sea
        // pickles have a physical shape but do NOT count as motion-blocking,
        // so they never raise the live OCEAN_FLOOR heightmap.
        pickleProps.setId("minecraft:sea_pickle").noOcclusion().noCollission();
        registerBlock("minecraft:sea_pickle", new SeaPickleBlockImpl(pickleProps));
    }
    CACTUS_FLOWER = createCactusFlowerBlock("minecraft:cactus_flower");
    SUGAR_CANE = createSugarCaneBlock("minecraft:sugar_cane");
    SWEET_BERRY_BUSH = createSweetBerryBushBlock("minecraft:sweet_berry_bush");
    LILY_PAD = createWaterlilyBlock("minecraft:lily_pad");
    FIREFLY_BUSH = createFireflyBushBlock("minecraft:firefly_bush");
    PUMPKIN = createSimpleBlock("minecraft:pumpkin");
    MELON = createSimpleBlock("minecraft:melon");
    COCOA = createCocoaBlock("minecraft:cocoa");
    {
        // Reference: Blocks.java MANGROVE_ROOTS - waterlogged (false), noOcclusion.
        Block::Properties rootsProps;
        rootsProps.setId("minecraft:mangrove_roots").noOcclusion();
        MANGROVE_ROOTS = new WaterloggedDefaultFalseBlockImpl(rootsProps);
        registerBlock("minecraft:mangrove_roots", MANGROVE_ROOTS);
    }
    OAK_SAPLING = createBushBlock("minecraft:oak_sapling", false);
    SPRUCE_SAPLING = createBushBlock("minecraft:spruce_sapling", false);
    BIRCH_SAPLING = createBushBlock("minecraft:birch_sapling", false);
    JUNGLE_SAPLING = createBushBlock("minecraft:jungle_sapling", false);
    ACACIA_SAPLING = createBushBlock("minecraft:acacia_sapling", false);
    CHERRY_SAPLING = createBushBlock("minecraft:cherry_sapling", false);
    DARK_OAK_SAPLING = createBushBlock("minecraft:dark_oak_sapling", false);
    PALE_OAK_SAPLING = createBushBlock("minecraft:pale_oak_sapling", false);
    {
        // Reference: MangrovePropaguleBlock - AGE_4 (0), STAGE (0), HANGING
        // (false), WATERLOGGED (false); not replaceable.
        Block::Properties propaguleProps;
        propaguleProps.setId("minecraft:mangrove_propagule").noCollission();
        MANGROVE_PROPAGULE = new MangrovePropaguleBlockImpl(propaguleProps);
        registerBlock("minecraft:mangrove_propagule", MANGROVE_PROPAGULE);
    }

    // =========================================================================
    // Dungeon blocks
    // =========================================================================
    SPAWNER = createNoOcclusionBlock("minecraft:spawner");
    {
        Block::Properties props;
        props.setId("minecraft:chest");
        CHEST = new ChestBlockImpl(props);
        registerBlock("minecraft:chest", CHEST);
    }
    {
        Block::Properties props;
        props.setId("minecraft:bee_nest");
        BEE_NEST = new BeehiveBlockImpl(props);
        registerBlock("minecraft:bee_nest", BEE_NEST);
    }

    // =========================================================================
    // Structure blocks
    // Reference: Used by mineshafts, shipwrecks, and other generated structures
    // =========================================================================
    createSimpleBlock("minecraft:oak_planks");
    createSimpleBlock("minecraft:spruce_planks");
    createSimpleBlock("minecraft:dark_oak_planks");
    createSimpleBlock("minecraft:spruce_stairs");
    createSimpleBlock("minecraft:dark_oak_stairs");
    createSimpleBlock("minecraft:spruce_fence");
    createSimpleBlock("minecraft:dark_oak_fence");
    createSimpleBlock("minecraft:oak_trapdoor");
    createNoCollisionBlock("minecraft:rail");
    createForceSolidOnNoCollisionBlock("minecraft:cobweb");
    createForceSolidOnNoOcclusionBlock("minecraft:iron_chain");
    createNoCollisionBlock("minecraft:wall_torch");

    // =========================================================================
    // Stairs - TEMPORARY: Using simple Block to avoid state system issues
    // TODO: Fix std::any_cast issue in StateDefinition and use StairBlock
    // =========================================================================
    OAK_STAIRS = reinterpret_cast<StairBlock*>(createSimpleBlock("minecraft:oak_stairs"));
    STONE_STAIRS = reinterpret_cast<StairBlock*>(createSimpleBlock("minecraft:stone_stairs"));
    COBBLESTONE_STAIRS = reinterpret_cast<StairBlock*>(createSimpleBlock("minecraft:cobblestone_stairs"));

    // =========================================================================
    // Slabs - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    OAK_SLAB = reinterpret_cast<SlabBlock*>(createSimpleBlock("minecraft:oak_slab"));
    STONE_SLAB = reinterpret_cast<SlabBlock*>(createSimpleBlock("minecraft:stone_slab"));
    COBBLESTONE_SLAB = reinterpret_cast<SlabBlock*>(createSimpleBlock("minecraft:cobblestone_slab"));

    // =========================================================================
    // Fences - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    OAK_FENCE = reinterpret_cast<FenceBlock*>(createSimpleBlock("minecraft:oak_fence"));
    NETHER_BRICK_FENCE = reinterpret_cast<FenceBlock*>(createSimpleBlock("minecraft:nether_brick_fence"));

    // =========================================================================
    // Doors - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    OAK_DOOR = reinterpret_cast<DoorBlock*>(createSimpleBlock("minecraft:oak_door"));
    IRON_DOOR = reinterpret_cast<DoorBlock*>(createSimpleBlock("minecraft:iron_door"));

    // =========================================================================
    // Walls - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    COBBLESTONE_WALL = reinterpret_cast<WallBlock*>(createSimpleBlock("minecraft:cobblestone_wall"));
    STONE_BRICK_WALL = reinterpret_cast<WallBlock*>(createSimpleBlock("minecraft:stone_brick_wall"));

    // =========================================================================
    // Leaves
    // =========================================================================
    OAK_LEAVES = createLeavesBlock("minecraft:oak_leaves");
    SPRUCE_LEAVES = createLeavesBlock("minecraft:spruce_leaves");
    BIRCH_LEAVES = createLeavesBlock("minecraft:birch_leaves");
    JUNGLE_LEAVES = createLeavesBlock("minecraft:jungle_leaves");
    ACACIA_LEAVES = createLeavesBlock("minecraft:acacia_leaves");
    DARK_OAK_LEAVES = createLeavesBlock("minecraft:dark_oak_leaves");
    AZALEA_LEAVES = createLeavesBlock("minecraft:azalea_leaves");
    FLOWERING_AZALEA_LEAVES = createLeavesBlock("minecraft:flowering_azalea_leaves");
    MANGROVE_LEAVES = createLeavesBlock("minecraft:mangrove_leaves");
    CHERRY_LEAVES = createLeavesBlock("minecraft:cherry_leaves");
    PALE_OAK_LEAVES = createLeavesBlock("minecraft:pale_oak_leaves");

    // =========================================================================
    // Logs
    // =========================================================================
    OAK_LOG = createLogBlock("minecraft:oak_log");
    SPRUCE_LOG = createLogBlock("minecraft:spruce_log");
    BIRCH_LOG = createLogBlock("minecraft:birch_log");
    JUNGLE_LOG = createLogBlock("minecraft:jungle_log");
    ACACIA_LOG = createLogBlock("minecraft:acacia_log");
    DARK_OAK_LOG = createLogBlock("minecraft:dark_oak_log");
    MANGROVE_LOG = createLogBlock("minecraft:mangrove_log");
    CHERRY_LOG = createLogBlock("minecraft:cherry_log");
    PALE_OAK_LOG = createLogBlock("minecraft:pale_oak_log");

    // =========================================================================
    // Stripped logs
    // =========================================================================
    STRIPPED_OAK_LOG = createLogBlock("minecraft:stripped_oak_log");
    STRIPPED_SPRUCE_LOG = createLogBlock("minecraft:stripped_spruce_log");
    STRIPPED_BIRCH_LOG = createLogBlock("minecraft:stripped_birch_log");
    STRIPPED_JUNGLE_LOG = createLogBlock("minecraft:stripped_jungle_log");
    STRIPPED_ACACIA_LOG = createLogBlock("minecraft:stripped_acacia_log");
    STRIPPED_DARK_OAK_LOG = createLogBlock("minecraft:stripped_dark_oak_log");
    STRIPPED_MANGROVE_LOG = createLogBlock("minecraft:stripped_mangrove_log");
    STRIPPED_CHERRY_LOG = createLogBlock("minecraft:stripped_cherry_log");
    STRIPPED_PALE_OAK_LOG = createLogBlock("minecraft:stripped_pale_oak_log");

    s_initialized = true;
}

bool minecraft::world::level::block::Blocks::isInitialized() {
    return s_initialized;
}

Block* minecraft::world::level::block::Blocks::getBlock(const std::string& name) {
    auto it = s_blocksByName.find(name);
    return it != s_blocksByName.end() ? it->second : nullptr;
}

BlockState* minecraft::world::level::block::Blocks::getDefaultState(const std::string& name) {
    Block* block = getBlock(name);
    return block ? block->defaultBlockState() : nullptr;
}

} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
