#pragma once

#include "BooleanProperty.h"
#include "IntegerProperty.h"
#include "EnumProperty.h"
#include "DirectionProperty.h"
#include "AxisProperty.h"
#include "Half.h"
#include "SlabType.h"
#include "StairsShape.h"
#include "AttachFace.h"
#include "WallSide.h"
#include "DoubleBlockHalf.h"
#include "BedPart.h"
#include "ChestType.h"
#include "DoorHingeSide.h"
#include "RedstoneSide.h"
#include "RailShape.h"
#include "ComparatorMode.h"
#include "PistonType.h"
#include "BellAttachType.h"
#include "BambooLeaves.h"
#include "Tilt.h"
#include "DripstoneThickness.h"
#include "SculkSensorPhase.h"
#include "StructureMode.h"
#include <memory>

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

/**
 * BlockStateProperties - Common block state properties
 * Reference: net/minecraft/world/level/block/state/properties/BlockStateProperties.java
 *
 * Contains static instances of commonly used properties.
 * Properties are created lazily on first access.
 */
class BlockStateProperties {
private:
    // Prevent instantiation
    BlockStateProperties() = delete;

public:
    // =========================================================================
    // Boolean Properties
    // =========================================================================

    static BooleanProperty* POWERED;
    static BooleanProperty* WATERLOGGED;
    static BooleanProperty* LIT;
    static BooleanProperty* OPEN;
    static BooleanProperty* PERSISTENT;
    static BooleanProperty* SNOWY;
    static BooleanProperty* HANGING;
    static BooleanProperty* OCCUPIED;
    static BooleanProperty* ATTACHED;
    static BooleanProperty* ENABLED;
    static BooleanProperty* EXTENDED;
    static BooleanProperty* TRIGGERED;

    // Directional connection properties
    static BooleanProperty* UP;
    static BooleanProperty* DOWN;
    static BooleanProperty* NORTH;
    static BooleanProperty* EAST;
    static BooleanProperty* SOUTH;
    static BooleanProperty* WEST;

    // =========================================================================
    // Direction Properties
    // =========================================================================

    static DirectionProperty* FACING;
    static DirectionProperty* HORIZONTAL_FACING;
    static DirectionProperty* VERTICAL_DIRECTION;

    // =========================================================================
    // Axis Properties
    // =========================================================================

    static AxisProperty* AXIS;
    static AxisProperty* HORIZONTAL_AXIS;

    // =========================================================================
    // Integer Properties
    // =========================================================================

    // Age properties for crops
    static IntegerProperty* AGE_1;
    static IntegerProperty* AGE_2;
    static IntegerProperty* AGE_3;
    static IntegerProperty* AGE_4;
    static IntegerProperty* AGE_5;
    static IntegerProperty* AGE_7;
    static IntegerProperty* AGE_15;
    static IntegerProperty* AGE_25;

    // Distance property for leaves
    static IntegerProperty* DISTANCE;

    // Level properties
    static IntegerProperty* LEVEL;
    static IntegerProperty* LEVEL_CAULDRON;
    static IntegerProperty* LEVEL_COMPOSTER;

    // Other integer properties
    static IntegerProperty* POWER;
    static IntegerProperty* MOISTURE;
    static IntegerProperty* LAYERS;
    static IntegerProperty* ROTATION_16;
    static IntegerProperty* NOTE;
    static IntegerProperty* STAGE;

    // =========================================================================
    // Enum Properties
    // Reference: BlockStateProperties.java lines 60-132
    // =========================================================================

    static EnumProperty<Half>* HALF;
    static EnumProperty<SlabType>* SLAB_TYPE;
    static EnumProperty<StairsShape>* STAIRS_SHAPE;
    static EnumProperty<AttachFace>* ATTACH_FACE;
    static EnumProperty<DoubleBlockHalf>* DOUBLE_BLOCK_HALF;
    static EnumProperty<BedPart>* BED_PART;
    static EnumProperty<ChestType>* CHEST_TYPE;
    static EnumProperty<DoorHingeSide>* DOOR_HINGE;
    static EnumProperty<PistonType>* PISTON_TYPE;
    static EnumProperty<ComparatorMode>* MODE_COMPARATOR;
    static EnumProperty<BambooLeaves>* BAMBOO_LEAVES;
    static EnumProperty<Tilt>* TILT;
    static EnumProperty<DripstoneThickness>* DRIPSTONE_THICKNESS;
    static EnumProperty<SculkSensorPhase>* SCULK_SENSOR_PHASE;
    static EnumProperty<StructureMode>* STRUCTUREBLOCK_MODE;
    static EnumProperty<BellAttachType>* BELL_ATTACHMENT;
    static EnumProperty<RailShape>* RAIL_SHAPE;
    static EnumProperty<RailShape>* RAIL_SHAPE_STRAIGHT;

    // Wall connection properties
    static EnumProperty<WallSide>* EAST_WALL;
    static EnumProperty<WallSide>* NORTH_WALL;
    static EnumProperty<WallSide>* SOUTH_WALL;
    static EnumProperty<WallSide>* WEST_WALL;

    // Redstone connection properties
    static EnumProperty<RedstoneSide>* EAST_REDSTONE;
    static EnumProperty<RedstoneSide>* NORTH_REDSTONE;
    static EnumProperty<RedstoneSide>* SOUTH_REDSTONE;
    static EnumProperty<RedstoneSide>* WEST_REDSTONE;

    // =========================================================================
    // Additional Integer Properties
    // Reference: BlockStateProperties.java lines 83-119
    // =========================================================================

    static IntegerProperty* BITES;
    static IntegerProperty* CANDLES;
    static IntegerProperty* DELAY;
    static IntegerProperty* EGGS;
    static IntegerProperty* HATCH;
    static IntegerProperty* LEVEL_FLOWING;
    static IntegerProperty* LEVEL_HONEY;
    static IntegerProperty* PICKLES;
    static IntegerProperty* STABILITY_DISTANCE;
    static IntegerProperty* RESPAWN_ANCHOR_CHARGES;
    static IntegerProperty* DUSTED;
    static IntegerProperty* FLOWER_AMOUNT;
    static IntegerProperty* SEGMENT_AMOUNT;

    // =========================================================================
    // Additional Boolean Properties
    // Reference: BlockStateProperties.java lines 11-45
    // =========================================================================

    static BooleanProperty* BOTTOM;
    static BooleanProperty* CONDITIONAL;
    static BooleanProperty* DISARMED;
    static BooleanProperty* DRAG;
    static BooleanProperty* EYE;
    static BooleanProperty* FALLING;
    static BooleanProperty* INVERTED;
    static BooleanProperty* IN_WALL;
    static BooleanProperty* LOCKED;
    static BooleanProperty* NATURAL;
    static BooleanProperty* SHORT_PISTON;
    static BooleanProperty* SHRIEKING;
    static BooleanProperty* SIGNAL_FIRE;
    static BooleanProperty* TIP;
    static BooleanProperty* UNSTABLE;
    static BooleanProperty* BERRIES;
    static BooleanProperty* BLOOM;
    static BooleanProperty* CAN_SUMMON;
    static BooleanProperty* HAS_BOTTLE_0;
    static BooleanProperty* HAS_BOTTLE_1;
    static BooleanProperty* HAS_BOTTLE_2;
    static BooleanProperty* HAS_RECORD;
    static BooleanProperty* HAS_BOOK;
    static BooleanProperty* CRACKED;
    static BooleanProperty* CRAFTING;
    static BooleanProperty* OMINOUS;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * Initialize all properties
     * Must be called before using any properties
     */
    static void initialize();

    /**
     * Check if properties have been initialized
     */
    static bool isInitialized();

private:
    static bool s_initialized;
};

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft

// Convenience namespace alias
namespace minecraft {
    using BlockStateProperties = world::level::block::state::properties::BlockStateProperties;
}
