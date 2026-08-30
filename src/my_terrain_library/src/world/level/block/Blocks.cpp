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

// Reference: StairBlock.java - FACING (horizontal, default north), HALF
// (default bottom), SHAPE (default straight), WATERLOGGED (default false).
// Stairs are not full blocks: noOcclusion so isSolidRender() is false
// (StructurePiece.reorient relies on this for chest facing).
class StairBlockImpl : public Block {
public:
    explicit StairBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::HALF, state::properties::Half(state::properties::Half::BOTTOM));
            defaultState = defaultState->setValue(*BlockStateProperties::STAIRS_SHAPE, state::properties::StairsShape(state::properties::StairsShape::STRAIGHT));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::HALF,
                    BlockStateProperties::STAIRS_SHAPE, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: SlabBlock.java - TYPE (default bottom), WATERLOGGED (false).
class SlabBlockImpl : public Block {
public:
    explicit SlabBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::SLAB_TYPE, state::properties::SlabType(state::properties::SlabType::BOTTOM));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::SLAB_TYPE, BlockStateProperties::WATERLOGGED);
    }
};

// One boolean property (pressure plates POWERED false, tnt UNSTABLE false,
// redstone torch LIT true, ...).
class SingleBoolBlockImpl : public Block {
public:
    SingleBoolBlockImpl(const Properties& properties,
                        state::properties::BooleanProperty* property,
                        bool defaultValue = false)
        : Block(properties), m_property(property) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*m_property, defaultValue));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(m_property);
    }

private:
    state::properties::BooleanProperty* m_property;
};

// Reference: BedBlock.java - FACING (north), PART (foot), OCCUPIED (false).
class BedBlockImpl : public Block {
public:
    explicit BedBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::BED_PART, state::properties::BedPart(state::properties::BedPart::FOOT));
            defaultState = defaultState->setValue(*BlockStateProperties::OCCUPIED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::BED_PART,
                    BlockStateProperties::OCCUPIED);
    }
};

// Reference: LadderBlock/WallSignBlock - FACING (north), WATERLOGGED (false).
class HorizontalWaterloggedBlockImpl : public Block {
public:
    explicit HorizontalWaterloggedBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: BrewingStandBlock.java - HAS_BOTTLE_0/1/2 (false).
class BrewingStandBlockImpl : public Block {
public:
    explicit BrewingStandBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HAS_BOTTLE_0, false);
            defaultState = defaultState->setValue(*BlockStateProperties::HAS_BOTTLE_1, false);
            defaultState = defaultState->setValue(*BlockStateProperties::HAS_BOTTLE_2, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HAS_BOTTLE_0, BlockStateProperties::HAS_BOTTLE_1,
                    BlockStateProperties::HAS_BOTTLE_2);
    }
};

// Reference: LayeredCauldronBlock.java - LEVEL (1-3, default 1).
class LayeredCauldronBlockImpl : public Block {
public:
    explicit LayeredCauldronBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::LEVEL_CAULDRON, 1));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LEVEL_CAULDRON);
    }
};

// Reference: StructureBlock.java - MODE (default load).
class StructureBlockImpl : public Block {
public:
    explicit StructureBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::STRUCTUREBLOCK_MODE,
                state::properties::StructureMode(state::properties::StructureMode::LOAD)));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::STRUCTUREBLOCK_MODE);
    }
};

// Reference: FurnaceBlock.java - FACING (north), LIT (false).
class FurnaceBlockImpl : public Block {
public:
    explicit FurnaceBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::LIT, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::LIT);
    }
};

// Reference: TrapDoorBlock.java - FACING (north), OPEN (false), HALF
// (bottom), POWERED (false), WATERLOGGED (false).
class TrapDoorBlockImpl : public Block {
public:
    explicit TrapDoorBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::OPEN, false);
            defaultState = defaultState->setValue(*BlockStateProperties::HALF, state::properties::Half(state::properties::Half::BOTTOM));
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::OPEN,
                    BlockStateProperties::HALF, BlockStateProperties::POWERED,
                    BlockStateProperties::WATERLOGGED);
    }
};

// Reference: BrushableBlock.java - DUSTED (0-3, default 0).
class BrushableBlockImpl : public Block {
public:
    explicit BrushableBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::DUSTED, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::DUSTED);
    }
};

// Reference: TripWireHookBlock.java - FACING (horizontal, north),
// ATTACHED (false), POWERED (false).
class TripWireHookBlockImpl : public Block {
public:
    explicit TripWireHookBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::ATTACHED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::ATTACHED,
                    BlockStateProperties::POWERED);
    }
};

// Reference: TripWireBlock.java - POWERED/ATTACHED/DISARMED + 4 sides, all false.
class TripWireBlockImpl : public Block {
public:
    explicit TripWireBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::ATTACHED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::DISARMED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::NORTH, false);
            defaultState = defaultState->setValue(*BlockStateProperties::EAST, false);
            defaultState = defaultState->setValue(*BlockStateProperties::SOUTH, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WEST, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::POWERED, BlockStateProperties::ATTACHED,
                    BlockStateProperties::DISARMED, BlockStateProperties::NORTH,
                    BlockStateProperties::EAST, BlockStateProperties::SOUTH,
                    BlockStateProperties::WEST);
    }
};

// Reference: FireBlock.java - AGE (0) + NORTH/EAST/SOUTH/WEST/UP (false).
class FireBlockImpl : public Block {
public:
    explicit FireBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::AGE_15, 0);
            defaultState = defaultState->setValue(*BlockStateProperties::NORTH, false);
            defaultState = defaultState->setValue(*BlockStateProperties::EAST, false);
            defaultState = defaultState->setValue(*BlockStateProperties::SOUTH, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WEST, false);
            defaultState = defaultState->setValue(*BlockStateProperties::UP, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_15, BlockStateProperties::NORTH,
                    BlockStateProperties::EAST, BlockStateProperties::SOUTH,
                    BlockStateProperties::WEST, BlockStateProperties::UP);
    }
};

// Reference: RedStoneWireBlock.java - 4 RedstoneSide sides (none) + POWER (0).
class RedStoneWireBlockImpl : public Block {
public:
    explicit RedStoneWireBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using state::properties::RedstoneSide;
            defaultState = defaultState->setValue(*BlockStateProperties::NORTH_REDSTONE, RedstoneSide(RedstoneSide::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::EAST_REDSTONE, RedstoneSide(RedstoneSide::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::SOUTH_REDSTONE, RedstoneSide(RedstoneSide::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::WEST_REDSTONE, RedstoneSide(RedstoneSide::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::POWER, 0);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::NORTH_REDSTONE, BlockStateProperties::EAST_REDSTONE,
                    BlockStateProperties::SOUTH_REDSTONE, BlockStateProperties::WEST_REDSTONE,
                    BlockStateProperties::POWER);
    }
};

// Reference: LeverBlock.java - FACE (wall), FACING (north), POWERED (false).
class LeverBlockImpl : public Block {
public:
    explicit LeverBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using state::properties::AttachFace;
            defaultState = defaultState->setValue(*BlockStateProperties::ATTACH_FACE, AttachFace(AttachFace::WALL));
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::ATTACH_FACE, BlockStateProperties::HORIZONTAL_FACING,
                    BlockStateProperties::POWERED);
    }
};

// Reference: EndPortalFrameBlock.java - FACING (horizontal, north), EYE (false).
class EndPortalFrameBlockImpl : public Block {
public:
    explicit EndPortalFrameBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::EYE, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::EYE);
    }
};

// Reference: FenceGateBlock.java - FACING (north), OPEN/POWERED/IN_WALL (false).
class FenceGateBlockImpl : public Block {
public:
    explicit FenceGateBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::OPEN, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::IN_WALL, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::OPEN,
                    BlockStateProperties::POWERED, BlockStateProperties::IN_WALL);
    }
};

// Reference: FarmBlock.java - MOISTURE (0-7, default 0).
class FarmBlockImpl : public Block {
public:
    explicit FarmBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::MOISTURE, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::MOISTURE);
    }
};

// Reference: CropBlock.java (wheat) - AGE (0-7, default 0).
class CropBlockImpl : public Block {
public:
    explicit CropBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::AGE_7, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AGE_7);
    }
};

// Reference: WallSkullBlock.java - HORIZONTAL_FACING (default NORTH) +
// POWERED (default false, from AbstractSkullBlock).
class WallSkullBlockImpl : public Block {
public:
    explicit WallSkullBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState
                ->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH)
                ->setValue(*BlockStateProperties::POWERED, false));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING);
        builder.add(BlockStateProperties::POWERED);
    }
};

// Reference: SaplingBlock.java - STAGE (0-1, default 0); keeps BushBlock
// placement/replacement semantics.
class SaplingBlockImpl : public BushBlock {
public:
    explicit SaplingBlockImpl(const Properties& properties) : BushBlock(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::STAGE, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::STAGE);
    }
};

// Single integer-property block (beetroots AGE_3, ...).
class SingleIntBlockImpl : public Block {
public:
    SingleIntBlockImpl(const Properties& properties,
                       state::properties::IntegerProperty* property, int defaultValue)
        : Block(properties), m_property(property), m_default(defaultValue) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*m_property, m_default));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(m_property);
    }

private:
    state::properties::IntegerProperty* m_property;
    int m_default;
};

// Reference: CopperBulbBlock.java - LIT (false), POWERED (false).
class CopperBulbBlockImpl : public Block {
public:
    explicit CopperBulbBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::LIT, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LIT, BlockStateProperties::POWERED);
    }
};

// Reference: CandleBlock.java - CANDLES (1-4, default 1), LIT, WATERLOGGED.
class CandleBlockImpl : public Block {
public:
    explicit CandleBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::CANDLES, 1);
            defaultState = defaultState->setValue(*BlockStateProperties::LIT, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::CANDLES, BlockStateProperties::LIT,
                    BlockStateProperties::WATERLOGGED);
    }
};

// Reference: RedstoneWallTorchBlock.java - FACING (north), LIT (TRUE).
class RedstoneWallTorchBlockImpl : public Block {
public:
    explicit RedstoneWallTorchBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::LIT, true);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::LIT);
    }
};

// Reference: ComparatorBlock.java - FACING (north), MODE (compare), POWERED.
class ComparatorBlockImpl : public Block {
public:
    explicit ComparatorBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using CM = state::properties::ComparatorMode;
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::MODE_COMPARATOR, CM(CM::COMPARE));
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::MODE_COMPARATOR,
                    BlockStateProperties::POWERED);
    }
};

// Reference: DecoratedPotBlock.java - CRACKED (false), FACING (north),
// WATERLOGGED (false).
class DecoratedPotBlockImpl : public Block {
public:
    explicit DecoratedPotBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::CRACKED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::CRACKED, BlockStateProperties::HORIZONTAL_FACING,
                    BlockStateProperties::WATERLOGGED);
    }
};

// Reference: HopperBlock.java - ENABLED (true), FACING (down).
class HopperBlockImpl : public Block {
public:
    explicit HopperBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::ENABLED, true);
            defaultState = defaultState->setValue(*BlockStateProperties::FACING, core::Direction::DOWN);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::ENABLED, BlockStateProperties::FACING);
    }
};

// Reference: NoteBlock.java - INSTRUMENT (harp), NOTE (0), POWERED (false).
class NoteBlockImpl : public Block {
public:
    explicit NoteBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using NBI = state::properties::NoteBlockInstrument;
            defaultState = defaultState->setValue(*BlockStateProperties::NOTEBLOCK_INSTRUMENT, NBI(NBI::HARP));
            defaultState = defaultState->setValue(*BlockStateProperties::NOTE, 0);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::NOTEBLOCK_INSTRUMENT, BlockStateProperties::NOTE,
                    BlockStateProperties::POWERED);
    }
};

// Reference: PistonHeadBlock.java - FACING (all 6, north), SHORT (false),
// TYPE (normal).
class PistonHeadBlockImpl : public Block {
public:
    explicit PistonHeadBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using PT = state::properties::PistonType;
            defaultState = defaultState->setValue(*BlockStateProperties::FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::SHORT_PISTON, false);
            defaultState = defaultState->setValue(*BlockStateProperties::PISTON_TYPE, PT(PT::DEFAULT));
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::FACING, BlockStateProperties::SHORT_PISTON,
                    BlockStateProperties::PISTON_TYPE);
    }
};

// Reference: SkullBlock.java - POWERED (false), ROTATION (0).
class SkullBlockImpl : public Block {
public:
    explicit SkullBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::ROTATION_16, 0);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::POWERED, BlockStateProperties::ROTATION_16);
    }
};

// Reference: TrialSpawnerBlock.java - OMINOUS (false), STATE (inactive).
class TrialSpawnerBlockImpl : public Block {
public:
    explicit TrialSpawnerBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using TSS = state::properties::TrialSpawnerState;
            defaultState = defaultState->setValue(*BlockStateProperties::OMINOUS, false);
            defaultState = defaultState->setValue(*BlockStateProperties::TRIAL_SPAWNER_STATE, TSS(TSS::INACTIVE));
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::OMINOUS, BlockStateProperties::TRIAL_SPAWNER_STATE);
    }
};

// Reference: VaultBlock.java - FACING (north), OMINOUS (false),
// STATE (inactive).
class VaultBlockImpl : public Block {
public:
    explicit VaultBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using VS = state::properties::VaultState;
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::OMINOUS, false);
            defaultState = defaultState->setValue(*BlockStateProperties::VAULT_STATE, VS(VS::INACTIVE));
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::OMINOUS,
                    BlockStateProperties::VAULT_STATE);
    }
};

// Reference: BarrelBlock.java - FACING (all 6, north), OPEN (false).
class BarrelBlockImpl : public Block {
public:
    explicit BarrelBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::OPEN, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::FACING, BlockStateProperties::OPEN);
    }
};

// Reference: BellBlock.java - ATTACHMENT (floor), FACING (north), POWERED.
class BellBlockImpl : public Block {
public:
    explicit BellBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using BAT = state::properties::BellAttachType;
            defaultState = defaultState->setValue(*BlockStateProperties::BELL_ATTACHMENT, BAT(BAT::FLOOR));
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::BELL_ATTACHMENT, BlockStateProperties::HORIZONTAL_FACING,
                    BlockStateProperties::POWERED);
    }
};

// Reference: CampfireBlock.java - FACING (north), LIT (TRUE), SIGNAL_FIRE,
// WATERLOGGED (false).
class CampfireBlockImpl : public Block {
public:
    explicit CampfireBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::LIT, true);
            defaultState = defaultState->setValue(*BlockStateProperties::SIGNAL_FIRE, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::LIT,
                    BlockStateProperties::SIGNAL_FIRE, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: ComposterBlock.java - LEVEL (0-8, default 0).
class ComposterBlockImpl : public Block {
public:
    explicit ComposterBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(*BlockStateProperties::LEVEL_COMPOSTER, 0));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::LEVEL_COMPOSTER);
    }
};

// Reference: GrindstoneBlock.java - FACE (wall), FACING (north).
class GrindstoneBlockImpl : public Block {
public:
    explicit GrindstoneBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using state::properties::AttachFace;
            defaultState = defaultState->setValue(*BlockStateProperties::ATTACH_FACE, AttachFace(AttachFace::WALL));
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::ATTACH_FACE, BlockStateProperties::HORIZONTAL_FACING);
    }
};

// Reference: LanternBlock.java - HANGING (false), WATERLOGGED (false).
class LanternBlockImpl : public Block {
public:
    explicit LanternBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HANGING, false);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HANGING, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: LecternBlock.java - FACING (north), HAS_BOOK/POWERED (false).
class LecternBlockImpl : public Block {
public:
    explicit LecternBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::HAS_BOOK, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::HAS_BOOK,
                    BlockStateProperties::POWERED);
    }
};

// Reference: PistonBaseBlock.java - FACING (all 6, north), EXTENDED (false).
class PistonBlockImpl : public Block {
public:
    explicit PistonBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::EXTENDED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::FACING, BlockStateProperties::EXTENDED);
    }
};

// Reference: RepeaterBlock.java - DELAY (1), FACING (north), LOCKED (false),
// POWERED (false).
class RepeaterBlockImpl : public Block {
public:
    explicit RepeaterBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::DELAY, 1);
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::LOCKED, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::DELAY, BlockStateProperties::HORIZONTAL_FACING,
                    BlockStateProperties::LOCKED, BlockStateProperties::POWERED);
    }
};

// Reference: DispenserBlock.java - FACING (all 6, north), TRIGGERED (false).
class DispenserBlockImpl : public Block {
public:
    explicit DispenserBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::TRIGGERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::FACING, BlockStateProperties::TRIGGERED);
    }
};

// Reference: RailBlock.java - SHAPE (default north_south), WATERLOGGED (false).
class RailBlockImpl : public Block {
public:
    explicit RailBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::RAIL_SHAPE,
                state::properties::RailShape(state::properties::RailShape::NORTH_SOUTH));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::RAIL_SHAPE, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: WallTorchBlock.java - FACING (horizontal, north).
class WallTorchBlockImpl : public Block {
public:
    explicit WallTorchBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            registerDefaultState(defaultState->setValue(
                *BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH));
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING);
    }
};

// Reference: ChainBlock.java - AXIS (default Y), WATERLOGGED (false).
class ChainBlockImpl : public Block {
public:
    explicit ChainBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::AXIS, core::Axis::Y);
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::AXIS, BlockStateProperties::WATERLOGGED);
    }
};

// Reference: DoorBlock.java - FACING (north), HALF (lower), HINGE (left),
// OPEN (false), POWERED (false).
class DoorBlockImpl : public Block {
public:
    explicit DoorBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            defaultState = defaultState->setValue(*BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
            defaultState = defaultState->setValue(*BlockStateProperties::DOUBLE_BLOCK_HALF,
                state::properties::DoubleBlockHalf(state::properties::DoubleBlockHalf::LOWER));
            defaultState = defaultState->setValue(*BlockStateProperties::DOOR_HINGE,
                state::properties::DoorHingeSide(state::properties::DoorHingeSide::LEFT));
            defaultState = defaultState->setValue(*BlockStateProperties::OPEN, false);
            defaultState = defaultState->setValue(*BlockStateProperties::POWERED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::DOUBLE_BLOCK_HALF,
                    BlockStateProperties::DOOR_HINGE, BlockStateProperties::OPEN,
                    BlockStateProperties::POWERED);
    }
};

// Reference: WallBlock.java - UP (true), EAST/NORTH/SOUTH/WEST (WallSide
// none), WATERLOGGED (false).
class WallBlockImpl : public Block {
public:
    explicit WallBlockImpl(const Properties& properties) : Block(properties) {
        rebuildStateDefinition();
        BlockState* defaultState = getStateDefinition().any();
        if (defaultState) {
            using WS = state::properties::WallSide;
            defaultState = defaultState->setValue(*BlockStateProperties::UP, true);
            defaultState = defaultState->setValue(*BlockStateProperties::EAST_WALL, WS(WS::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::NORTH_WALL, WS(WS::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::SOUTH_WALL, WS(WS::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::WEST_WALL, WS(WS::NONE));
            defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
            registerDefaultState(defaultState);
        }
    }

protected:
    void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
        BlockStateProperties::initialize();
        builder.add(BlockStateProperties::UP, BlockStateProperties::EAST_WALL,
                    BlockStateProperties::NORTH_WALL, BlockStateProperties::SOUTH_WALL,
                    BlockStateProperties::WEST_WALL, BlockStateProperties::WATERLOGGED);
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
        if (::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                below, "minecraft:mushroom_grow_block")) {
            return true;
        }
        // Java fallback: getRawBrightness(pos, 0) < 13 && mayPlaceOn
        // (= belowState.isSolidRender). During worldgen the raw brightness is
        // 15 in skylight dimensions (overworld: fallback NEVER fires,
        // gate-proven) and 0 in the nether/end (fallback ALWAYS fires).
        if (level.hasSkyLight()) {
            return false;
        }
        return below != nullptr && below->isSolidRender();
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
    // Reference: Blocks.java:1601 - basalt is a RotatedPillarBlock (AXIS, default y).
    createLogBlock("minecraft:basalt");
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
    // Reference: SaplingBlock - EVERY sapling carries STAGE (0-1), not just
    // dark oak. acacia_sapling as a plain BushBlock made the template loader
    // ABORT worldgen on village/savanna/houses/savanna_library_1 (its palette
    // lists stage) - found by terrain/tests/template_sweep.cpp.
    auto createSapling = [](const char* name) -> Block* {
        Block::Properties saplingProps;
        saplingProps.setId(name).replaceableByTrees();
        auto* sapling = new SaplingBlockImpl(saplingProps);
        registerBlock(name, sapling);
        return sapling;
    };
    OAK_SAPLING = createSapling("minecraft:oak_sapling");
    SPRUCE_SAPLING = createSapling("minecraft:spruce_sapling");
    BIRCH_SAPLING = createSapling("minecraft:birch_sapling");
    JUNGLE_SAPLING = createSapling("minecraft:jungle_sapling");
    ACACIA_SAPLING = createSapling("minecraft:acacia_sapling");
    CHERRY_SAPLING = createSapling("minecraft:cherry_sapling");
    {
        // Reference: SaplingBlock - STAGE property (mansion templates carry it);
        // same BushBlock property flags as createBushBlock(name, false).
        Block::Properties saplingProps;
        saplingProps.setId("minecraft:dark_oak_sapling").replaceableByTrees();
        auto darkOakSapling = new SaplingBlockImpl(saplingProps);
        registerBlock("minecraft:dark_oak_sapling", darkOakSapling);
        DARK_OAK_SAPLING = darkOakSapling;
    }
    PALE_OAK_SAPLING = createSapling("minecraft:pale_oak_sapling");
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
        // Structure-piece blocks (B6). Stairs/cauldron/flower pot are not
        // full blocks: noOcclusion => isSolidRender false, matching Java.
        Block::Properties stairProps;
        stairProps.setId("minecraft:spruce_stairs").noOcclusion();
        registerBlock("minecraft:spruce_stairs", new StairBlockImpl(stairProps));

        Block::Properties cauldronProps;
        cauldronProps.setId("minecraft:cauldron").noOcclusion();
        registerBlock("minecraft:cauldron", new Block(cauldronProps));

        createSimpleBlock("minecraft:crafting_table");

        Block::Properties potProps;
        potProps.setId("minecraft:potted_red_mushroom").noOcclusion();
        registerBlock("minecraft:potted_red_mushroom", new Block(potProps));

        // Desert pyramid blocks (B6).
        createSimpleBlock("minecraft:cut_sandstone");
        createSimpleBlock("minecraft:chiseled_sandstone");
        createSimpleBlock("minecraft:blue_terracotta");

        Block::Properties ssStairProps;
        ssStairProps.setId("minecraft:sandstone_stairs").noOcclusion();
        registerBlock("minecraft:sandstone_stairs", new StairBlockImpl(ssStairProps));

        Block::Properties ssSlabProps;
        ssSlabProps.setId("minecraft:sandstone_slab").noOcclusion();
        registerBlock("minecraft:sandstone_slab", new SlabBlockImpl(ssSlabProps));

        Block::Properties plateProps;
        plateProps.setId("minecraft:stone_pressure_plate").noCollission();
        registerBlock("minecraft:stone_pressure_plate",
                      new SingleBoolBlockImpl(plateProps, BlockStateProperties::POWERED));

        Block::Properties tntProps;
        tntProps.setId("minecraft:tnt");
        registerBlock("minecraft:tnt",
                      new SingleBoolBlockImpl(tntProps, BlockStateProperties::UNSTABLE));

        Block::Properties susSandProps;
        susSandProps.setId("minecraft:suspicious_sand");
        registerBlock("minecraft:suspicious_sand", new BrushableBlockImpl(susSandProps));

        // Ruined portal blocks (B6).
        createSimpleBlock("minecraft:gold_block");
        createSimpleBlock("minecraft:crying_obsidian");
        createSimpleBlock("minecraft:jigsaw");
        for (const char* slab : {"minecraft:smooth_stone_slab", "minecraft:stone_brick_slab",
                                 "minecraft:mossy_stone_brick_slab"}) {
            Block::Properties props;
            props.setId(slab).noOcclusion();
            registerBlock(slab, new SlabBlockImpl(props));
        }
        {
            Block::Properties msbStairProps;
            msbStairProps.setId("minecraft:mossy_stone_brick_stairs").noOcclusion();
            registerBlock("minecraft:mossy_stone_brick_stairs", new StairBlockImpl(msbStairProps));
        }

        // Ocean ruin blocks (B6).
        createSimpleBlock("minecraft:bricks");
        createSimpleBlock("minecraft:light_blue_terracotta");
        createSimpleBlock("minecraft:obsidian");
        createSimpleBlock("minecraft:polished_diorite");
        createSimpleBlock("minecraft:polished_granite");
        createSimpleBlock("minecraft:prismarine");
        createSimpleBlock("minecraft:sea_lantern");
        {
            Block::Properties sbStairProps;
            sbStairProps.setId("minecraft:stone_brick_stairs").noOcclusion();
            registerBlock("minecraft:stone_brick_stairs", new StairBlockImpl(sbStairProps));
        }
        {
            Block::Properties susGravelProps;
            susGravelProps.setId("minecraft:suspicious_gravel");
            registerBlock("minecraft:suspicious_gravel", new BrushableBlockImpl(susGravelProps));
        }

        // Ocean monument blocks (B6).
        createSimpleBlock("minecraft:prismarine_bricks");
        createSimpleBlock("minecraft:dark_prismarine");
        createSimpleBlock("minecraft:wet_sponge");

        // Woodland mansion blocks (B6).
        for (const char* wool : {"minecraft:black_wool", "minecraft:blue_wool",
                                 "minecraft:brown_wool", "minecraft:cyan_wool",
                                 "minecraft:gray_wool", "minecraft:green_wool",
                                 "minecraft:light_blue_wool", "minecraft:light_gray_wool",
                                 "minecraft:lime_wool", "minecraft:orange_wool",
                                 "minecraft:red_wool", "minecraft:white_wool",
                                 "minecraft:yellow_wool"}) {
            createSimpleBlock(wool);
        }
        for (const char* carpet : {"minecraft:black_carpet", "minecraft:blue_carpet",
                                   "minecraft:brown_carpet", "minecraft:cyan_carpet",
                                   "minecraft:gray_carpet", "minecraft:green_carpet",
                                   "minecraft:light_blue_carpet", "minecraft:lime_carpet",
                                   "minecraft:magenta_carpet", "minecraft:pink_carpet",
                                   "minecraft:purple_carpet", "minecraft:yellow_carpet"}) {
            Block::Properties carpetProps;
            carpetProps.setId(carpet).noOcclusion();
            registerBlock(carpet, new Block(carpetProps));
        }
        createNoOcclusionBlock("minecraft:glass");
        {
            Block::Properties paneProps;
            paneProps.setId("minecraft:glass_pane").noOcclusion();
            registerBlock("minecraft:glass_pane", new FenceBlock(paneProps));
        }
        createSimpleBlock("minecraft:infested_cobblestone");
        createSimpleBlock("minecraft:lapis_block");
        createSimpleBlock("minecraft:diamond_block");
        {
            Block::Properties trappedProps;
            trappedProps.setId("minecraft:trapped_chest");
            registerBlock("minecraft:trapped_chest", new ChestBlockImpl(trappedProps));
        }
        for (const char* banner : {"minecraft:black_wall_banner", "minecraft:gray_wall_banner",
                                   "minecraft:light_gray_wall_banner"}) {
            Block::Properties bannerProps;
            bannerProps.setId(banner).noCollission();
            registerBlock(banner, new WallTorchBlockImpl(bannerProps));
        }
        for (const char* stem : {"minecraft:attached_melon_stem",
                                 "minecraft:attached_pumpkin_stem"}) {
            Block::Properties stemProps;
            stemProps.setId(stem).noCollission();
            registerBlock(stem, new WallTorchBlockImpl(stemProps));
        }
        {
            Block::Properties pumpkinProps;
            pumpkinProps.setId("minecraft:carved_pumpkin");
            registerBlock("minecraft:carved_pumpkin", new WallTorchBlockImpl(pumpkinProps));
        }
        {
            Block::Properties anvilProps;
            anvilProps.setId("minecraft:damaged_anvil").noOcclusion();
            registerBlock("minecraft:damaged_anvil", new WallTorchBlockImpl(anvilProps));
        }
        {
            Block::Properties gateProps;
            gateProps.setId("minecraft:dark_oak_fence_gate").noOcclusion();
            registerBlock("minecraft:dark_oak_fence_gate", new FenceGateBlockImpl(gateProps));
        }
        {
            Block::Properties farmProps;
            farmProps.setId("minecraft:farmland").noOcclusion();
            registerBlock("minecraft:farmland", new FarmBlockImpl(farmProps));
        }
        {
            Block::Properties wheatProps;
            wheatProps.setId("minecraft:wheat").noCollission();
            registerBlock("minecraft:wheat", new CropBlockImpl(wheatProps));
        }
        for (const char* pot : {"minecraft:potted_allium", "minecraft:potted_azure_bluet",
                                "minecraft:potted_birch_sapling", "minecraft:potted_blue_orchid",
                                "minecraft:potted_dandelion", "minecraft:potted_oxeye_daisy",
                                "minecraft:potted_poppy", "minecraft:potted_red_tulip",
                                "minecraft:potted_white_tulip"}) {
            createNoOcclusionBlock(pot);
        }

        // Ancient city / trail ruins / trial chambers blocks (B7 batch 2).
        for (const char* simple : {"minecraft:chiseled_deepslate", "minecraft:chiseled_tuff",
                                   "minecraft:chiseled_tuff_bricks", "minecraft:coal_block",
                                   "minecraft:cobbled_deepslate", "minecraft:copper_block",
                                   "minecraft:cyan_terracotta", "minecraft:gray_terracotta",
                                   "minecraft:oxidized_cut_copper", "minecraft:polished_deepslate",
                                   "minecraft:polished_tuff", "minecraft:red_concrete",
                                   "minecraft:redstone_block", "minecraft:reinforced_deepslate",
                                   "minecraft:tuff_bricks", "minecraft:waxed_chiseled_copper",
                                   "minecraft:waxed_copper_block", "minecraft:waxed_cut_copper",
                                   "minecraft:waxed_oxidized_chiseled_copper",
                                   "minecraft:waxed_oxidized_copper",
                                   "minecraft:waxed_oxidized_cut_copper",
                                   "minecraft:white_concrete"}) {
            createSimpleBlock(simple);
        }
        for (const char* glass : {"minecraft:black_stained_glass",
                                  "minecraft:brown_stained_glass",
                                  "minecraft:light_gray_stained_glass",
                                  "minecraft:white_stained_glass"}) {
            createNoOcclusionBlock(glass);
        }
        createNoOcclusionBlock("minecraft:flower_pot");
    {
        // Reference: Blocks.java:1510 - soul_fire is replaceable, no collision.
        Block::Properties soulFireProps;
        soulFireProps.setId("minecraft:soul_fire").noCollission().replaceable();
        registerBlock("minecraft:soul_fire", new Block(soulFireProps));
    }
    {
        // Reference: Blocks.java:1509 - fire is a FireBlock (AGE + N/E/S/W/UP),
        // no collision, replaceable.
        Block::Properties fireProps;
        fireProps.setId("minecraft:fire").noCollission().replaceable();
        registerBlock("minecraft:fire", new FireBlockImpl(fireProps));
    }
    // Reference: Blocks.java - glowstone/nether ores/ancient_debris are propertyless.
    createSimpleBlock("minecraft:glowstone");
    createSimpleBlock("minecraft:nether_gold_ore");
    createSimpleBlock("minecraft:nether_quartz_ore");
    createSimpleBlock("minecraft:ancient_debris");
    {
        // DriedGhastBlock: HORIZONTAL_FACING (north) + hydration (0) + WATERLOGGED.
        class DriedGhastBlockImpl : public Block {
        public:
            explicit DriedGhastBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    defaultState = defaultState->setValue(
                        *BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
                    defaultState = defaultState->setValue(*BlockStateProperties::DRIED_GHAST_HYDRATION, 0);
                    defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
                    registerDefaultState(defaultState);
                }
            }
        protected:
            void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
                BlockStateProperties::initialize();
                builder.add(BlockStateProperties::HORIZONTAL_FACING,
                            BlockStateProperties::DRIED_GHAST_HYDRATION,
                            BlockStateProperties::WATERLOGGED);
            }
        };
        Block::Properties ghastProps;
        ghastProps.setId("minecraft:dried_ghast").noOcclusion();
        registerBlock("minecraft:dried_ghast", new DriedGhastBlockImpl(ghastProps));
    }
    // Bastion / end city template blocks:
    createSimpleBlock("minecraft:purpur_block");
    createSimpleBlock("minecraft:quartz_block");
    createSimpleBlock("minecraft:smooth_quartz");
    createSimpleBlock("minecraft:end_stone_bricks");
    createLogBlock("minecraft:purpur_pillar");
    {
        Block::Properties purpurStairProps;
        purpurStairProps.setId("minecraft:purpur_stairs").noOcclusion();
        registerBlock("minecraft:purpur_stairs", new StairBlockImpl(purpurStairProps));
    }
    for (const char* slab : {"minecraft:purpur_slab", "minecraft:smooth_quartz_slab"}) {
        Block::Properties slabProps;
        slabProps.setId(slab).noOcclusion();
        registerBlock(slab, new SlabBlockImpl(slabProps));
    }
    createNoOcclusionBlock("minecraft:magenta_stained_glass");
    {
        // WallBannerBlock: HORIZONTAL_FACING only (same shape as wall torch).
        Block::Properties bannerProps;
        bannerProps.setId("minecraft:magenta_wall_banner").noCollission();
        registerBlock("minecraft:magenta_wall_banner", new WallTorchBlockImpl(bannerProps));
    }
    {
        // EndRodBlock (RodBlock): FACING 6-dir, default UP.
        class EndRodBlockImpl : public Block {
        public:
            explicit EndRodBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    registerDefaultState(defaultState->setValue(
                        *BlockStateProperties::FACING, core::Direction::UP));
                }
            }
        protected:
            void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
                BlockStateProperties::initialize();
                builder.add(BlockStateProperties::FACING);
            }
        };
        Block::Properties rodProps;
        rodProps.setId("minecraft:end_rod").noOcclusion();
        registerBlock("minecraft:end_rod", new EndRodBlockImpl(rodProps));
    }
    {
        // EnderChestBlock: HORIZONTAL_FACING (north) + WATERLOGGED (false).
        class EnderChestBlockImpl : public Block {
        public:
            explicit EnderChestBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    defaultState = defaultState->setValue(
                        *BlockStateProperties::HORIZONTAL_FACING, core::Direction::NORTH);
                    defaultState = defaultState->setValue(*BlockStateProperties::WATERLOGGED, false);
                    registerDefaultState(defaultState);
                }
            }
        protected:
            void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
                BlockStateProperties::initialize();
                builder.add(BlockStateProperties::HORIZONTAL_FACING, BlockStateProperties::WATERLOGGED);
            }
        };
        Block::Properties enderProps;
        enderProps.setId("minecraft:ender_chest").noOcclusion();
        registerBlock("minecraft:ender_chest", new EnderChestBlockImpl(enderProps));
    }
    // Nether fortress blocks:
    createSimpleBlock("minecraft:nether_bricks");
    {
        Block::Properties nbStairProps;
        nbStairProps.setId("minecraft:nether_brick_stairs").noOcclusion();
        registerBlock("minecraft:nether_brick_stairs", new StairBlockImpl(nbStairProps));
    }
    {
        // NetherWartBlock: AGE 0-3 (default 0), noCollision.
        class NetherWartBlockImpl : public Block {
        public:
            explicit NetherWartBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    registerDefaultState(defaultState->setValue(*BlockStateProperties::AGE_3, 0));
                }
            }
        protected:
            void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
                BlockStateProperties::initialize();
                builder.add(BlockStateProperties::AGE_3);
            }
        };
        Block::Properties wartProps;
        wartProps.setId("minecraft:nether_wart").noCollission();
        registerBlock("minecraft:nether_wart", new NetherWartBlockImpl(wartProps));
    }
    // BlackstoneReplaceProcessor outputs (ruined_portal_nether):
    createSimpleBlock("minecraft:polished_blackstone");
    for (const char* stairs : {"minecraft:blackstone_stairs",
                               "minecraft:polished_blackstone_stairs",
                               "minecraft:polished_blackstone_brick_stairs"}) {
        Block::Properties stairProps;
        stairProps.setId(stairs).noOcclusion();
        registerBlock(stairs, new StairBlockImpl(stairProps));
    }
    for (const char* slab : {"minecraft:blackstone_slab",
                             "minecraft:polished_blackstone_slab",
                             "minecraft:polished_blackstone_brick_slab"}) {
        Block::Properties slabProps;
        slabProps.setId(slab).noOcclusion();
        registerBlock(slab, new SlabBlockImpl(slabProps));
    }
    // Huge-fungus blocks: stems are RotatedPillarBlocks (AXIS), shroomlight simple.
    createLogBlock("minecraft:crimson_stem");
    createLogBlock("minecraft:warped_stem");
    createSimpleBlock("minecraft:shroomlight");
    // Nether plants - Reference: Blocks.java:2161-2177. RootsBlock/FungusBlock/
    // NetherSproutsBlock are propertyless, noCollision; canSurvive = mayPlaceOn
    // below in #nylium || soul_soil || (#dirt || farmland via VegetationBlock).
    // (FungusBlock also lists mycelium explicitly, but mycelium is in #dirt.)
    {
        class NetherPlantBlockImpl : public BushBlock {
        public:
            explicit NetherPlantBlockImpl(const Properties& properties) : BushBlock(properties) {}
        protected:
            bool mayPlaceOn(BlockState* stateBelow) const override {
                return ::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                           stateBelow, "minecraft:nylium") ||
                       (stateBelow && stateBelow->getIdentifier() == "minecraft:soul_soil") ||
                       BushBlock::mayPlaceOn(stateBelow);
            }
        };
        // roots + sprouts are .replaceable() in Java; fungus is NOT.
        for (const char* name : {"minecraft:crimson_roots", "minecraft:warped_roots",
                                 "minecraft:nether_sprouts"}) {
            Block::Properties plantProps;
            plantProps.setId(name).noCollission().replaceable();
            registerBlock(name, new NetherPlantBlockImpl(plantProps));
        }
        for (const char* name : {"minecraft:crimson_fungus", "minecraft:warped_fungus"}) {
            Block::Properties plantProps;
            plantProps.setId(name).noCollission();
            registerBlock(name, new NetherPlantBlockImpl(plantProps));
        }
    }
    createNoCollisionBlock("minecraft:weeping_vines_plant");
    createNoCollisionBlock("minecraft:twisting_vines_plant");
    // End blocks - Reference: Blocks.java:1954-1965
    {
        // ChorusPlantBlock extends PipeBlock: 6 bools (N/E/S/W/UP/DOWN), all false.
        class ChorusPlantBlockImpl : public Block {
        public:
            explicit ChorusPlantBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    defaultState = defaultState->setValue(*BlockStateProperties::NORTH, false);
                    defaultState = defaultState->setValue(*BlockStateProperties::EAST, false);
                    defaultState = defaultState->setValue(*BlockStateProperties::SOUTH, false);
                    defaultState = defaultState->setValue(*BlockStateProperties::WEST, false);
                    defaultState = defaultState->setValue(*BlockStateProperties::UP, false);
                    defaultState = defaultState->setValue(*BlockStateProperties::DOWN, false);
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
        Block::Properties chorusProps;
        chorusProps.setId("minecraft:chorus_plant").noOcclusion();
        registerBlock("minecraft:chorus_plant", new ChorusPlantBlockImpl(chorusProps));

        // ChorusFlowerBlock: AGE_5 (0 default).
        class ChorusFlowerBlockImpl : public Block {
        public:
            explicit ChorusFlowerBlockImpl(const Properties& properties) : Block(properties) {
                rebuildStateDefinition();
                BlockState* defaultState = getStateDefinition().any();
                if (defaultState) {
                    registerDefaultState(defaultState->setValue(*BlockStateProperties::AGE_5, 0));
                }
            }
        protected:
            void createBlockStateDefinition(typename StateDefinition<Block, BlockState>::Builder& builder) override {
                BlockStateProperties::initialize();
                builder.add(BlockStateProperties::AGE_5);
            }
        };
        Block::Properties flowerProps;
        flowerProps.setId("minecraft:chorus_flower").noOcclusion();
        registerBlock("minecraft:chorus_flower", new ChorusFlowerBlockImpl(flowerProps));

        Block::Properties gatewayProps;
        gatewayProps.setId("minecraft:end_gateway").noCollission();
        registerBlock("minecraft:end_gateway", new Block(gatewayProps));
    }
    {
        // WeepingVinesBlock/TwistingVinesBlock (GrowingPlantHeadBlock): AGE 0-25.
        Block::Properties weepingProps;
        weepingProps.setId("minecraft:weeping_vines").noCollission();
        registerBlock("minecraft:weeping_vines", new Age25HeadBlock(weepingProps));
        Block::Properties twistingProps;
        twistingProps.setId("minecraft:twisting_vines").noCollission();
        registerBlock("minecraft:twisting_vines", new Age25HeadBlock(twistingProps));
    }
        createLogBlock("minecraft:mangrove_wood");
        createLogBlock("minecraft:polished_basalt");
        for (const char* stairs : {"minecraft:brick_stairs", "minecraft:cobbled_deepslate_stairs",
                                   "minecraft:deepslate_brick_stairs",
                                   "minecraft:deepslate_tile_stairs", "minecraft:mud_brick_stairs",
                                   "minecraft:polished_deepslate_stairs",
                                   "minecraft:waxed_cut_copper_stairs",
                                   "minecraft:waxed_oxidized_cut_copper_stairs"}) {
            Block::Properties props;
            props.setId(stairs).noOcclusion();
            registerBlock(stairs, new StairBlockImpl(props));
        }
        for (const char* slab : {"minecraft:brick_slab", "minecraft:cobbled_deepslate_slab",
                                 "minecraft:deepslate_brick_slab", "minecraft:mud_brick_slab",
                                 "minecraft:polished_deepslate_slab",
                                 "minecraft:polished_tuff_slab",
                                 "minecraft:waxed_cut_copper_slab",
                                 "minecraft:waxed_oxidized_cut_copper_slab"}) {
            Block::Properties props;
            props.setId(slab).noOcclusion();
            registerBlock(slab, new SlabBlockImpl(props));
        }
        for (const char* wall : {"minecraft:brick_wall", "minecraft:cobbled_deepslate_wall",
                                 "minecraft:deepslate_brick_wall",
                                 "minecraft:deepslate_tile_wall", "minecraft:mud_brick_wall",
                                 "minecraft:polished_deepslate_wall",
                                 // Blackstone family (BlackstoneReplaceProcessor targets)
                                 "minecraft:blackstone_wall",
                                 "minecraft:polished_blackstone_brick_wall"}) {
            Block::Properties props;
            props.setId(wall).noOcclusion();
            registerBlock(wall, new WallBlockImpl(props));
        }
        for (const char* bed : {"minecraft:black_bed", "minecraft:brown_bed",
                                "minecraft:gray_bed", "minecraft:light_blue_bed",
                                "minecraft:light_gray_bed", "minecraft:magenta_bed",
                                "minecraft:pink_bed"}) {
            Block::Properties props;
            props.setId(bed).noOcclusion();
            registerBlock(bed, new BedBlockImpl(props));
        }
        for (const char* glazed : {"minecraft:black_glazed_terracotta",
                                   "minecraft:cyan_glazed_terracotta",
                                   "minecraft:light_gray_glazed_terracotta",
                                   "minecraft:red_glazed_terracotta"}) {
            Block::Properties props;
            props.setId(glazed);
            registerBlock(glazed, new WallTorchBlockImpl(props));
        }
        for (const char* trapdoor : {"minecraft:iron_trapdoor",
                                     "minecraft:oxidized_copper_trapdoor",
                                     "minecraft:waxed_oxidized_copper_trapdoor"}) {
            Block::Properties props;
            props.setId(trapdoor).noOcclusion();
            registerBlock(trapdoor, new TrapDoorBlockImpl(props));
        }
        for (const char* door : {"minecraft:waxed_copper_door",
                                 "minecraft:waxed_oxidized_copper_door"}) {
            Block::Properties props;
            props.setId(door).noOcclusion();
            registerBlock(door, new DoorBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:oak_button").noCollission();
            registerBlock("minecraft:oak_button", new LeverBlockImpl(props));
        }
        for (const char* candle : {"minecraft:candle", "minecraft:red_candle",
                                   "minecraft:white_candle"}) {
            Block::Properties props;
            props.setId(candle).noOcclusion();
            registerBlock(candle, new CandleBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:redstone_lamp");
            registerBlock("minecraft:redstone_lamp",
                          new SingleBoolBlockImpl(props, BlockStateProperties::LIT));
        }
        {
            Block::Properties props;
            props.setId("minecraft:redstone_wall_torch").noCollission();
            registerBlock("minecraft:redstone_wall_torch", new RedstoneWallTorchBlockImpl(props));
        }
        for (const char* grate : {"minecraft:waxed_copper_grate",
                                  "minecraft:waxed_oxidized_copper_grate"}) {
            Block::Properties props;
            props.setId(grate).noOcclusion();
            registerBlock(grate, new SingleBoolBlockImpl(props, BlockStateProperties::WATERLOGGED));
        }
        {
            Block::Properties props;
            props.setId("minecraft:comparator").noOcclusion();
            registerBlock("minecraft:comparator", new ComparatorBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:decorated_pot").noOcclusion();
            registerBlock("minecraft:decorated_pot", new DecoratedPotBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:hopper").noOcclusion();
            registerBlock("minecraft:hopper", new HopperBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:note_block");
            registerBlock("minecraft:note_block", new NoteBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:piston_head").noOcclusion();
            registerBlock("minecraft:piston_head", new PistonHeadBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:skeleton_skull").noOcclusion();
            registerBlock("minecraft:skeleton_skull", new SkullBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:target");
            registerBlock("minecraft:target",
                          new SingleIntBlockImpl(props, BlockStateProperties::POWER, 0));
        }
        {
            Block::Properties props;
            props.setId("minecraft:trial_spawner").noOcclusion();
            registerBlock("minecraft:trial_spawner", new TrialSpawnerBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:vault").noOcclusion();
            registerBlock("minecraft:vault", new VaultBlockImpl(props));
        }

        // Processor-list output/input blocks (B7) - all 40 lists surveyed.
        for (const char* simple : {"minecraft:chiseled_polished_blackstone",
                                   "minecraft:cracked_deepslate_bricks",
                                   "minecraft:cracked_deepslate_tiles",
                                   "minecraft:cracked_polished_blackstone_bricks",
                                   "minecraft:deepslate_bricks",
                                   "minecraft:deepslate_tiles",
                                   "minecraft:gilded_blackstone",
                                   "minecraft:mud_bricks",
                                   "minecraft:packed_mud",
                                   "minecraft:polished_blackstone_bricks"}) {
            createSimpleBlock(simple);
        }
        {
            Block::Properties props;
            props.setId("minecraft:deepslate_tile_slab").noOcclusion();
            registerBlock("minecraft:deepslate_tile_slab", new SlabBlockImpl(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:brown_stained_glass_pane").noOcclusion();
            registerBlock("minecraft:brown_stained_glass_pane", new FenceBlock(props));
        }
        {
            Block::Properties props;
            props.setId("minecraft:soul_lantern").noOcclusion();
            registerBlock("minecraft:soul_lantern", new LanternBlockImpl(props));
        }
        for (const char* crop : {"minecraft:carrots", "minecraft:potatoes"}) {
            Block::Properties props;
            props.setId(crop).noCollission();
            registerBlock(crop, new CropBlockImpl(props));
        }
        {
            // Reference: BeetrootBlock - AGE 0-3.
            Block::Properties props;
            props.setId("minecraft:beetroots").noCollission();
            registerBlock("minecraft:beetroots",
                          new SingleIntBlockImpl(props, BlockStateProperties::AGE_3, 0));
        }
        for (const char* bulb : {"minecraft:waxed_copper_bulb",
                                 "minecraft:waxed_exposed_copper_bulb",
                                 "minecraft:waxed_oxidized_copper_bulb",
                                 "minecraft:waxed_weathered_copper_bulb"}) {
            // Reference: CopperBulbBlock - LIT (false), POWERED (false).
            Block::Properties props;
            props.setId(bulb);
            registerBlock(bulb, new CopperBulbBlockImpl(props));
        }

        // Village + pillager outpost blocks (B7). Grouped by impl class;
        // jack_o_lantern: pile_pumpkin weighted provider (CarvedPumpkinBlock
        // pattern - FACING only).
        {
            Block::Properties jackProps;
            jackProps.setId("minecraft:jack_o_lantern");
            registerBlock("minecraft:jack_o_lantern", new WallTorchBlockImpl(jackProps));
        }
        // property sets match the template palette scan exactly.
        createSimpleBlock("minecraft:acacia_planks");
        createSimpleBlock("minecraft:cartography_table");
        createSimpleBlock("minecraft:fletching_table");
        createSimpleBlock("minecraft:smithing_table");
        createSimpleBlock("minecraft:lime_terracotta");
        createSimpleBlock("minecraft:smooth_sandstone");
        createSimpleBlock("minecraft:smooth_stone");
        {
            // Reference: DirtPathBlock - 15/16 slab (sturdiness rules in
            // BlockState.cpp).
            Block::Properties pathProps;
            pathProps.setId("minecraft:dirt_path").noOcclusion();
            registerBlock("minecraft:dirt_path", new Block(pathProps));
        }
        {
            Block::Properties carpetProps;
            carpetProps.setId("minecraft:orange_carpet").noOcclusion();
            registerBlock("minecraft:orange_carpet", new Block(carpetProps));
        }
        createNoOcclusionBlock("minecraft:potted_dead_bush");
        createNoOcclusionBlock("minecraft:potted_spruce_sapling");
        createLogBlock("minecraft:hay_block");
        createLogBlock("minecraft:acacia_wood");
        createLogBlock("minecraft:spruce_wood");
        createLogBlock("minecraft:stripped_oak_wood");
        createLogBlock("minecraft:stripped_spruce_wood");
        for (const char* stairs : {"minecraft:acacia_stairs", "minecraft:diorite_stairs",
                                   "minecraft:granite_stairs", "minecraft:mossy_cobblestone_stairs",
                                   "minecraft:smooth_sandstone_stairs"}) {
            Block::Properties props;
            props.setId(stairs).noOcclusion();
            registerBlock(stairs, new StairBlockImpl(props));
        }
        for (const char* slab : {"minecraft:acacia_slab", "minecraft:diorite_slab",
                                 "minecraft:mossy_cobblestone_slab",
                                 "minecraft:smooth_sandstone_slab"}) {
            Block::Properties props;
            props.setId(slab).noOcclusion();
            registerBlock(slab, new SlabBlockImpl(props));
        }
        for (const char* wall : {"minecraft:diorite_wall", "minecraft:granite_wall",
                                 "minecraft:mossy_cobblestone_wall", "minecraft:sandstone_wall"}) {
            Block::Properties props;
            props.setId(wall).noOcclusion();
            registerBlock(wall, new WallBlockImpl(props));
        }
        for (const char* fence : {"minecraft:acacia_fence"}) {
            Block::Properties props;
            props.setId(fence).noOcclusion();
            registerBlock(fence, new FenceBlock(props));
        }
        for (const char* pane : {"minecraft:orange_stained_glass_pane",
                                 "minecraft:white_stained_glass_pane",
                                 "minecraft:yellow_stained_glass_pane"}) {
            Block::Properties props;
            props.setId(pane).noOcclusion();
            registerBlock(pane, new FenceBlock(props));
        }
        for (const char* gate : {"minecraft:acacia_fence_gate", "minecraft:jungle_fence_gate",
                                 "minecraft:oak_fence_gate", "minecraft:spruce_fence_gate"}) {
            Block::Properties props;
            props.setId(gate).noOcclusion();
            registerBlock(gate, new FenceGateBlockImpl(props));
        }
        {
            Block::Properties doorProps;
            doorProps.setId("minecraft:acacia_door").noOcclusion();
            registerBlock("minecraft:acacia_door", new DoorBlockImpl(doorProps));
        }
        for (const char* bed : {"minecraft:blue_bed", "minecraft:cyan_bed",
                                "minecraft:green_bed", "minecraft:lime_bed",
                                "minecraft:orange_bed", "minecraft:purple_bed",
                                "minecraft:white_bed", "minecraft:yellow_bed"}) {
            Block::Properties props;
            props.setId(bed).noOcclusion();
            registerBlock(bed, new BedBlockImpl(props));
        }
        for (const char* banner : {"minecraft:brown_wall_banner",
                                   "minecraft:white_wall_banner"}) {
            Block::Properties props;
            props.setId(banner).noCollission();
            registerBlock(banner, new WallTorchBlockImpl(props));
        }
        // ALL 16 glazed terracottas carry HORIZONTAL_FACING
        // (GlazedTerracottaBlock). purple was once a propertyless simple
        // block, which made the template loader ABORT worldgen on the LARGE
        // underwater ruins (underwater_ruin/big_*_2) - found by
        // terrain/tests/template_sweep.cpp after a field crash.
        {
            // Reference: WallSkullBlock (FACING north) + AbstractSkullBlock
            // (POWERED false). end_city/ship's palette needs it - unregistered
            // it ABORTED worldgen when an end-city ship placed (template_sweep).
            Block::Properties props;
            props.setId("minecraft:dragon_wall_head");
            registerBlock("minecraft:dragon_wall_head", new WallSkullBlockImpl(props));
        }
        for (const char* facingOnly : {"minecraft:light_blue_glazed_terracotta",
                                       "minecraft:lime_glazed_terracotta",
                                       "minecraft:orange_glazed_terracotta",
                                       "minecraft:white_glazed_terracotta",
                                       "minecraft:yellow_glazed_terracotta",
                                       "minecraft:purple_glazed_terracotta",
                                       "minecraft:magenta_glazed_terracotta",
                                       "minecraft:pink_glazed_terracotta",
                                       "minecraft:gray_glazed_terracotta",
                                       "minecraft:blue_glazed_terracotta",
                                       "minecraft:brown_glazed_terracotta",
                                       "minecraft:green_glazed_terracotta",
                                       "minecraft:loom"}) {
            Block::Properties props;
            props.setId(facingOnly);
            registerBlock(facingOnly, new WallTorchBlockImpl(props));
        }
        {
            Block::Properties cutterProps;
            cutterProps.setId("minecraft:stonecutter").noOcclusion();
            registerBlock("minecraft:stonecutter", new WallTorchBlockImpl(cutterProps));
        }
        for (const char* plate : {"minecraft:acacia_pressure_plate",
                                  "minecraft:oak_pressure_plate",
                                  "minecraft:spruce_pressure_plate"}) {
            Block::Properties props;
            props.setId(plate).noCollission();
            registerBlock(plate, new SingleBoolBlockImpl(props, BlockStateProperties::POWERED));
        }
        {
            Block::Properties buttonProps;
            buttonProps.setId("minecraft:jungle_button").noCollission();
            registerBlock("minecraft:jungle_button", new LeverBlockImpl(buttonProps));
        }
        for (const char* furnaceLike : {"minecraft:blast_furnace", "minecraft:smoker"}) {
            Block::Properties props;
            props.setId(furnaceLike);
            registerBlock(furnaceLike, new FurnaceBlockImpl(props));
        }
        {
            Block::Properties signProps;
            signProps.setId("minecraft:spruce_wall_sign").noCollission();
            registerBlock("minecraft:spruce_wall_sign", new HorizontalWaterloggedBlockImpl(signProps));
        }
        for (const char* stem : {"minecraft:melon_stem", "minecraft:pumpkin_stem"}) {
            Block::Properties props;
            props.setId(stem).noCollission();
            registerBlock(stem, new CropBlockImpl(props));
        }
        {
            Block::Properties barrelProps;
            barrelProps.setId("minecraft:barrel");
            registerBlock("minecraft:barrel", new BarrelBlockImpl(barrelProps));
        }
        {
            Block::Properties bellProps;
            bellProps.setId("minecraft:bell").noOcclusion();
            registerBlock("minecraft:bell", new BellBlockImpl(bellProps));
        }
        {
            Block::Properties campfireProps;
            campfireProps.setId("minecraft:campfire").noOcclusion();
            registerBlock("minecraft:campfire", new CampfireBlockImpl(campfireProps));
        }
        {
            Block::Properties composterProps;
            composterProps.setId("minecraft:composter").noOcclusion();
            registerBlock("minecraft:composter", new ComposterBlockImpl(composterProps));
        }
        {
            Block::Properties grindProps;
            grindProps.setId("minecraft:grindstone").noOcclusion();
            registerBlock("minecraft:grindstone", new GrindstoneBlockImpl(grindProps));
        }
        {
            Block::Properties lanternProps;
            lanternProps.setId("minecraft:lantern").noOcclusion();
            registerBlock("minecraft:lantern", new LanternBlockImpl(lanternProps));
        }
        {
            Block::Properties lecternProps;
            lecternProps.setId("minecraft:lectern").noOcclusion();
            registerBlock("minecraft:lectern", new LecternBlockImpl(lecternProps));
        }

        // Stronghold blocks (B6). Java properties: torch/end_portal are
        // noCollission; stone_button shares the LeverBlock property set
        // (FACE wall / FACING north / POWERED false).
        createSimpleBlock("minecraft:bookshelf");
        createNoCollisionBlock("minecraft:torch");
        createNoCollisionBlock("minecraft:end_portal");
        {
            Block::Properties buttonProps;
            buttonProps.setId("minecraft:stone_button").noCollission();
            registerBlock("minecraft:stone_button", new LeverBlockImpl(buttonProps));
        }
        {
            Block::Properties frameProps;
            frameProps.setId("minecraft:end_portal_frame").noOcclusion();
            registerBlock("minecraft:end_portal_frame", new EndPortalFrameBlockImpl(frameProps));
        }

        // Jungle temple blocks (B6).
        createSimpleBlock("minecraft:chiseled_stone_bricks");

        Block::Properties hookProps;
        hookProps.setId("minecraft:tripwire_hook").noCollission();
        registerBlock("minecraft:tripwire_hook", new TripWireHookBlockImpl(hookProps));

        Block::Properties wireProps;
        wireProps.setId("minecraft:tripwire").noCollission();
        registerBlock("minecraft:tripwire", new TripWireBlockImpl(wireProps));

        Block::Properties redstoneProps;
        redstoneProps.setId("minecraft:redstone_wire").noCollission();
        registerBlock("minecraft:redstone_wire", new RedStoneWireBlockImpl(redstoneProps));

        Block::Properties leverProps;
        leverProps.setId("minecraft:lever").noCollission();
        registerBlock("minecraft:lever", new LeverBlockImpl(leverProps));

        Block::Properties pistonProps;
        pistonProps.setId("minecraft:sticky_piston");
        registerBlock("minecraft:sticky_piston", new PistonBlockImpl(pistonProps));

        Block::Properties repeaterProps;
        repeaterProps.setId("minecraft:repeater").noCollission();
        registerBlock("minecraft:repeater", new RepeaterBlockImpl(repeaterProps));

        Block::Properties dispenserProps;
        dispenserProps.setId("minecraft:dispenser");
        registerBlock("minecraft:dispenser", new DispenserBlockImpl(dispenserProps));

        // Igloo blocks (B6 template placement).
        createSimpleBlock("minecraft:stone_bricks");
        createSimpleBlock("minecraft:mossy_stone_bricks");
        createSimpleBlock("minecraft:cracked_stone_bricks");
        createSimpleBlock("minecraft:infested_stone_bricks");
        createSimpleBlock("minecraft:infested_mossy_stone_bricks");
        createSimpleBlock("minecraft:infested_chiseled_stone_bricks");
        createSimpleBlock("minecraft:polished_andesite");
        for (const char* carpet : {"minecraft:white_carpet", "minecraft:light_gray_carpet",
                                   "minecraft:red_carpet"}) {
            Block::Properties carpetProps;
            carpetProps.setId(carpet).noOcclusion();
            registerBlock(carpet, new Block(carpetProps));
        }
        {
            Block::Properties potProps;
            potProps.setId("minecraft:potted_cactus").noOcclusion();
            registerBlock("minecraft:potted_cactus", new Block(potProps));
        }
        {
            Block::Properties bedProps;
            bedProps.setId("minecraft:red_bed").noOcclusion();
            registerBlock("minecraft:red_bed", new BedBlockImpl(bedProps));
        }
        {
            Block::Properties ladderProps;
            ladderProps.setId("minecraft:ladder").noCollission();
            registerBlock("minecraft:ladder", new HorizontalWaterloggedBlockImpl(ladderProps));
        }
        {
            Block::Properties signProps;
            signProps.setId("minecraft:oak_wall_sign").noCollission();
            registerBlock("minecraft:oak_wall_sign", new HorizontalWaterloggedBlockImpl(signProps));
        }
        {
            // Iron bars share the fence/pane connection property set.
            Block::Properties barsProps;
            barsProps.setId("minecraft:iron_bars").noOcclusion();
            registerBlock("minecraft:iron_bars", new FenceBlock(barsProps));
        }
        {
            Block::Properties brewingProps;
            brewingProps.setId("minecraft:brewing_stand").noOcclusion();
            registerBlock("minecraft:brewing_stand", new BrewingStandBlockImpl(brewingProps));
        }
        {
            Block::Properties cauldronProps2;
            cauldronProps2.setId("minecraft:water_cauldron").noOcclusion();
            registerBlock("minecraft:water_cauldron", new LayeredCauldronBlockImpl(cauldronProps2));
        }
        {
            Block::Properties torchProps2;
            torchProps2.setId("minecraft:redstone_torch").noCollission();
            registerBlock("minecraft:redstone_torch",
                          new SingleBoolBlockImpl(torchProps2, BlockStateProperties::LIT, true));
        }
        {
            Block::Properties sbProps;
            sbProps.setId("minecraft:structure_block");
            registerBlock("minecraft:structure_block", new StructureBlockImpl(sbProps));
        }
        {
            Block::Properties furnaceProps;
            furnaceProps.setId("minecraft:furnace");
            registerBlock("minecraft:furnace", new FurnaceBlockImpl(furnaceProps));
        }
        {
            Block::Properties slabProps2;
            slabProps2.setId("minecraft:spruce_slab").noOcclusion();
            registerBlock("minecraft:spruce_slab", new SlabBlockImpl(slabProps2));
        }
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
    // spruce_stairs is registered above with full stair properties (B6).
    {
        Block::Properties doStairProps;
        doStairProps.setId("minecraft:dark_oak_stairs").noOcclusion();
        registerBlock("minecraft:dark_oak_stairs", new StairBlockImpl(doStairProps));
    }
    {
        Block::Properties spruceFenceProps;
        spruceFenceProps.setId("minecraft:spruce_fence").noOcclusion();
        registerBlock("minecraft:spruce_fence", new FenceBlock(spruceFenceProps));
    }
    {
        // Mineshaft blocks with real properties (B6).
        Block::Properties doFenceProps;
        doFenceProps.setId("minecraft:dark_oak_fence").noOcclusion();
        registerBlock("minecraft:dark_oak_fence", new FenceBlock(doFenceProps));

        Block::Properties railProps;
        railProps.setId("minecraft:rail").noCollission();
        registerBlock("minecraft:rail", new RailBlockImpl(railProps));

        Block::Properties chainProps;
        chainProps.setId("minecraft:iron_chain").forceSolidOn().noOcclusion();
        registerBlock("minecraft:iron_chain", new ChainBlockImpl(chainProps));

        Block::Properties torchProps;
        torchProps.setId("minecraft:wall_torch").noCollission();
        registerBlock("minecraft:wall_torch", new WallTorchBlockImpl(torchProps));
    }
    {
        Block::Properties trapdoorProps;
        trapdoorProps.setId("minecraft:oak_trapdoor").noOcclusion();
        registerBlock("minecraft:oak_trapdoor", new TrapDoorBlockImpl(trapdoorProps));
    }
    createForceSolidOnNoCollisionBlock("minecraft:cobweb");

    // =========================================================================
    // Stairs - TEMPORARY: Using simple Block to avoid state system issues
    // TODO: Fix std::any_cast issue in StateDefinition and use StairBlock
    // =========================================================================
    {
        Block::Properties oakStairProps;
        oakStairProps.setId("minecraft:oak_stairs").noOcclusion();
        Block* oakStairs = new StairBlockImpl(oakStairProps);
        registerBlock("minecraft:oak_stairs", oakStairs);
        OAK_STAIRS = reinterpret_cast<StairBlock*>(oakStairs);
    }
    STONE_STAIRS = reinterpret_cast<StairBlock*>(createSimpleBlock("minecraft:stone_stairs"));
    {
        // Real stair properties (B6 jungle temple places these).
        Block::Properties cobbleStairProps;
        cobbleStairProps.setId("minecraft:cobblestone_stairs").noOcclusion();
        Block* cobbleStairs = new StairBlockImpl(cobbleStairProps);
        registerBlock("minecraft:cobblestone_stairs", cobbleStairs);
        COBBLESTONE_STAIRS = reinterpret_cast<StairBlock*>(cobbleStairs);
    }

    // =========================================================================
    // Slabs - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    {
        Block::Properties oakSlabProps;
        oakSlabProps.setId("minecraft:oak_slab").noOcclusion();
        Block* oakSlab = new SlabBlockImpl(oakSlabProps);
        registerBlock("minecraft:oak_slab", oakSlab);
        OAK_SLAB = reinterpret_cast<SlabBlock*>(oakSlab);
    }
    {
        Block::Properties stoneSlabProps;
        stoneSlabProps.setId("minecraft:stone_slab").noOcclusion();
        Block* stoneSlab = new SlabBlockImpl(stoneSlabProps);
        registerBlock("minecraft:stone_slab", stoneSlab);
        STONE_SLAB = reinterpret_cast<SlabBlock*>(stoneSlab);
    }
    {
        Block::Properties cobbleSlabProps;
        cobbleSlabProps.setId("minecraft:cobblestone_slab").noOcclusion();
        Block* cobbleSlab = new SlabBlockImpl(cobbleSlabProps);
        registerBlock("minecraft:cobblestone_slab", cobbleSlab);
        COBBLESTONE_SLAB = reinterpret_cast<SlabBlock*>(cobbleSlab);
    }
    {
        // Shipwreck wood families (B6).
        createSimpleBlock("minecraft:birch_planks");
        createSimpleBlock("minecraft:jungle_planks");
        for (const char* fence : {"minecraft:birch_fence", "minecraft:jungle_fence"}) {
            Block::Properties props;
            props.setId(fence).noOcclusion();
            registerBlock(fence, new FenceBlock(props));
        }
        for (const char* slab : {"minecraft:birch_slab", "minecraft:dark_oak_slab",
                                 "minecraft:jungle_slab"}) {
            Block::Properties props;
            props.setId(slab).noOcclusion();
            registerBlock(slab, new SlabBlockImpl(props));
        }
        for (const char* stairs : {"minecraft:birch_stairs", "minecraft:jungle_stairs"}) {
            Block::Properties props;
            props.setId(stairs).noOcclusion();
            registerBlock(stairs, new StairBlockImpl(props));
        }
        for (const char* trapdoor : {"minecraft:dark_oak_trapdoor", "minecraft:jungle_trapdoor",
                                     "minecraft:spruce_trapdoor"}) {
            Block::Properties props;
            props.setId(trapdoor).noOcclusion();
            registerBlock(trapdoor, new TrapDoorBlockImpl(props));
        }
        for (const char* door : {"minecraft:dark_oak_door", "minecraft:jungle_door",
                                 "minecraft:spruce_door"}) {
            Block::Properties props;
            props.setId(door).noOcclusion();
            registerBlock(door, new DoorBlockImpl(props));
        }
    }

    // =========================================================================
    // Fences - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    {
        // Real FenceBlock instances (connection + waterlogged properties in
        // the dump; noOcclusion => isSolidRender false like Java fences).
        Block::Properties oakFenceProps;
        oakFenceProps.setId("minecraft:oak_fence").noOcclusion();
        OAK_FENCE = new FenceBlock(oakFenceProps);
        registerBlock("minecraft:oak_fence", OAK_FENCE);

        Block::Properties netherFenceProps;
        netherFenceProps.setId("minecraft:nether_brick_fence").noOcclusion();
        NETHER_BRICK_FENCE = new FenceBlock(netherFenceProps);
        registerBlock("minecraft:nether_brick_fence", NETHER_BRICK_FENCE);
    }

    // =========================================================================
    // Doors - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    {
        Block::Properties oakDoorProps;
        oakDoorProps.setId("minecraft:oak_door").noOcclusion();
        Block* oakDoor = new DoorBlockImpl(oakDoorProps);
        registerBlock("minecraft:oak_door", oakDoor);
        OAK_DOOR = reinterpret_cast<DoorBlock*>(oakDoor);
    }
    {
        Block::Properties ironDoorProps;
        ironDoorProps.setId("minecraft:iron_door").noOcclusion();
        Block* ironDoor = new DoorBlockImpl(ironDoorProps);
        registerBlock("minecraft:iron_door", ironDoor);
        IRON_DOOR = reinterpret_cast<DoorBlock*>(ironDoor);
    }

    // =========================================================================
    // Walls - TEMPORARY: Using simple Block to avoid state system issues
    // =========================================================================
    {
        Block::Properties cobbleWallProps;
        cobbleWallProps.setId("minecraft:cobblestone_wall").noOcclusion();
        Block* cobbleWall = new WallBlockImpl(cobbleWallProps);
        registerBlock("minecraft:cobblestone_wall", cobbleWall);
        COBBLESTONE_WALL = reinterpret_cast<WallBlock*>(cobbleWall);
    }
    {
        Block::Properties sbWallProps;
        sbWallProps.setId("minecraft:stone_brick_wall").noOcclusion();
        Block* sbWall = new WallBlockImpl(sbWallProps);
        registerBlock("minecraft:stone_brick_wall", sbWall);
        STONE_BRICK_WALL = reinterpret_cast<WallBlock*>(sbWall);

        Block::Properties msbWallProps;
        msbWallProps.setId("minecraft:mossy_stone_brick_wall").noOcclusion();
        registerBlock("minecraft:mossy_stone_brick_wall", new WallBlockImpl(msbWallProps));
    }

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
