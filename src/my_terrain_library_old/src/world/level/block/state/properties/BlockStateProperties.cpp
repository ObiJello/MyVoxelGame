#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {
namespace state {
namespace properties {

// Static member definitions
bool BlockStateProperties::s_initialized = false;

// Boolean properties
BooleanProperty* BlockStateProperties::POWERED = nullptr;
BooleanProperty* BlockStateProperties::WATERLOGGED = nullptr;
BooleanProperty* BlockStateProperties::LIT = nullptr;
BooleanProperty* BlockStateProperties::OPEN = nullptr;
BooleanProperty* BlockStateProperties::PERSISTENT = nullptr;
BooleanProperty* BlockStateProperties::SNOWY = nullptr;
BooleanProperty* BlockStateProperties::HANGING = nullptr;
BooleanProperty* BlockStateProperties::OCCUPIED = nullptr;
BooleanProperty* BlockStateProperties::ATTACHED = nullptr;
BooleanProperty* BlockStateProperties::ENABLED = nullptr;
BooleanProperty* BlockStateProperties::EXTENDED = nullptr;
BooleanProperty* BlockStateProperties::TRIGGERED = nullptr;

// Directional connection properties
BooleanProperty* BlockStateProperties::UP = nullptr;
BooleanProperty* BlockStateProperties::DOWN = nullptr;
BooleanProperty* BlockStateProperties::NORTH = nullptr;
BooleanProperty* BlockStateProperties::EAST = nullptr;
BooleanProperty* BlockStateProperties::SOUTH = nullptr;
BooleanProperty* BlockStateProperties::WEST = nullptr;

// Direction properties
DirectionProperty* BlockStateProperties::FACING = nullptr;
DirectionProperty* BlockStateProperties::HORIZONTAL_FACING = nullptr;
DirectionProperty* BlockStateProperties::VERTICAL_DIRECTION = nullptr;

// Axis properties
AxisProperty* BlockStateProperties::AXIS = nullptr;
AxisProperty* BlockStateProperties::HORIZONTAL_AXIS = nullptr;

// Age properties
IntegerProperty* BlockStateProperties::AGE_1 = nullptr;
IntegerProperty* BlockStateProperties::AGE_2 = nullptr;
IntegerProperty* BlockStateProperties::AGE_3 = nullptr;
IntegerProperty* BlockStateProperties::AGE_4 = nullptr;
IntegerProperty* BlockStateProperties::AGE_5 = nullptr;
IntegerProperty* BlockStateProperties::AGE_7 = nullptr;
IntegerProperty* BlockStateProperties::AGE_15 = nullptr;
IntegerProperty* BlockStateProperties::AGE_25 = nullptr;

// Other integer properties
IntegerProperty* BlockStateProperties::DISTANCE = nullptr;
IntegerProperty* BlockStateProperties::LEVEL = nullptr;
IntegerProperty* BlockStateProperties::LEVEL_CAULDRON = nullptr;
IntegerProperty* BlockStateProperties::LEVEL_COMPOSTER = nullptr;
IntegerProperty* BlockStateProperties::POWER = nullptr;
IntegerProperty* BlockStateProperties::MOISTURE = nullptr;
IntegerProperty* BlockStateProperties::LAYERS = nullptr;
IntegerProperty* BlockStateProperties::ROTATION_16 = nullptr;
IntegerProperty* BlockStateProperties::NOTE = nullptr;
IntegerProperty* BlockStateProperties::STAGE = nullptr;

// Enum properties
EnumProperty<Half>* BlockStateProperties::HALF = nullptr;
EnumProperty<SlabType>* BlockStateProperties::SLAB_TYPE = nullptr;
EnumProperty<StairsShape>* BlockStateProperties::STAIRS_SHAPE = nullptr;
EnumProperty<AttachFace>* BlockStateProperties::ATTACH_FACE = nullptr;
EnumProperty<DoubleBlockHalf>* BlockStateProperties::DOUBLE_BLOCK_HALF = nullptr;
EnumProperty<BedPart>* BlockStateProperties::BED_PART = nullptr;
EnumProperty<ChestType>* BlockStateProperties::CHEST_TYPE = nullptr;
EnumProperty<DoorHingeSide>* BlockStateProperties::DOOR_HINGE = nullptr;
EnumProperty<PistonType>* BlockStateProperties::PISTON_TYPE = nullptr;
EnumProperty<ComparatorMode>* BlockStateProperties::MODE_COMPARATOR = nullptr;
EnumProperty<BambooLeaves>* BlockStateProperties::BAMBOO_LEAVES = nullptr;
EnumProperty<Tilt>* BlockStateProperties::TILT = nullptr;
EnumProperty<DripstoneThickness>* BlockStateProperties::DRIPSTONE_THICKNESS = nullptr;
EnumProperty<SculkSensorPhase>* BlockStateProperties::SCULK_SENSOR_PHASE = nullptr;
EnumProperty<StructureMode>* BlockStateProperties::STRUCTUREBLOCK_MODE = nullptr;
EnumProperty<BellAttachType>* BlockStateProperties::BELL_ATTACHMENT = nullptr;
EnumProperty<RailShape>* BlockStateProperties::RAIL_SHAPE = nullptr;
EnumProperty<RailShape>* BlockStateProperties::RAIL_SHAPE_STRAIGHT = nullptr;
EnumProperty<WallSide>* BlockStateProperties::EAST_WALL = nullptr;
EnumProperty<WallSide>* BlockStateProperties::NORTH_WALL = nullptr;
EnumProperty<WallSide>* BlockStateProperties::SOUTH_WALL = nullptr;
EnumProperty<WallSide>* BlockStateProperties::WEST_WALL = nullptr;
EnumProperty<RedstoneSide>* BlockStateProperties::EAST_REDSTONE = nullptr;
EnumProperty<RedstoneSide>* BlockStateProperties::NORTH_REDSTONE = nullptr;
EnumProperty<RedstoneSide>* BlockStateProperties::SOUTH_REDSTONE = nullptr;
EnumProperty<RedstoneSide>* BlockStateProperties::WEST_REDSTONE = nullptr;

// Additional integer properties
IntegerProperty* BlockStateProperties::BITES = nullptr;
IntegerProperty* BlockStateProperties::CANDLES = nullptr;
IntegerProperty* BlockStateProperties::DELAY = nullptr;
IntegerProperty* BlockStateProperties::EGGS = nullptr;
IntegerProperty* BlockStateProperties::HATCH = nullptr;
IntegerProperty* BlockStateProperties::LEVEL_FLOWING = nullptr;
IntegerProperty* BlockStateProperties::LEVEL_HONEY = nullptr;
IntegerProperty* BlockStateProperties::PICKLES = nullptr;
IntegerProperty* BlockStateProperties::STABILITY_DISTANCE = nullptr;
IntegerProperty* BlockStateProperties::RESPAWN_ANCHOR_CHARGES = nullptr;
IntegerProperty* BlockStateProperties::DUSTED = nullptr;
IntegerProperty* BlockStateProperties::FLOWER_AMOUNT = nullptr;
IntegerProperty* BlockStateProperties::SEGMENT_AMOUNT = nullptr;

// Additional boolean properties
BooleanProperty* BlockStateProperties::BOTTOM = nullptr;
BooleanProperty* BlockStateProperties::CONDITIONAL = nullptr;
BooleanProperty* BlockStateProperties::DISARMED = nullptr;
BooleanProperty* BlockStateProperties::DRAG = nullptr;
BooleanProperty* BlockStateProperties::EYE = nullptr;
BooleanProperty* BlockStateProperties::FALLING = nullptr;
BooleanProperty* BlockStateProperties::INVERTED = nullptr;
BooleanProperty* BlockStateProperties::IN_WALL = nullptr;
BooleanProperty* BlockStateProperties::LOCKED = nullptr;
BooleanProperty* BlockStateProperties::NATURAL = nullptr;
BooleanProperty* BlockStateProperties::SHORT_PISTON = nullptr;
BooleanProperty* BlockStateProperties::SHRIEKING = nullptr;
BooleanProperty* BlockStateProperties::SIGNAL_FIRE = nullptr;
BooleanProperty* BlockStateProperties::TIP = nullptr;
BooleanProperty* BlockStateProperties::UNSTABLE = nullptr;
BooleanProperty* BlockStateProperties::BERRIES = nullptr;
BooleanProperty* BlockStateProperties::BLOOM = nullptr;
BooleanProperty* BlockStateProperties::CAN_SUMMON = nullptr;
BooleanProperty* BlockStateProperties::HAS_BOTTLE_0 = nullptr;
BooleanProperty* BlockStateProperties::HAS_BOTTLE_1 = nullptr;
BooleanProperty* BlockStateProperties::HAS_BOTTLE_2 = nullptr;
BooleanProperty* BlockStateProperties::HAS_RECORD = nullptr;
BooleanProperty* BlockStateProperties::HAS_BOOK = nullptr;
BooleanProperty* BlockStateProperties::CRACKED = nullptr;
BooleanProperty* BlockStateProperties::CRAFTING = nullptr;
BooleanProperty* BlockStateProperties::OMINOUS = nullptr;

// Storage for owned properties (to prevent memory leaks)
namespace {
    std::vector<std::unique_ptr<PropertyBase>> s_ownedProperties;

    template<typename T>
    T* own(std::unique_ptr<T> ptr) {
        T* raw = ptr.get();
        s_ownedProperties.push_back(std::move(ptr));
        return raw;
    }
}

void BlockStateProperties::initialize() {
    if (s_initialized) return;

    // Boolean properties
    // Reference: BlockStateProperties.java lines 11-45
    POWERED = own(BooleanProperty::create("powered"));
    WATERLOGGED = own(BooleanProperty::create("waterlogged"));
    LIT = own(BooleanProperty::create("lit"));
    OPEN = own(BooleanProperty::create("open"));
    PERSISTENT = own(BooleanProperty::create("persistent"));
    SNOWY = own(BooleanProperty::create("snowy"));
    HANGING = own(BooleanProperty::create("hanging"));
    OCCUPIED = own(BooleanProperty::create("occupied"));
    ATTACHED = own(BooleanProperty::create("attached"));
    ENABLED = own(BooleanProperty::create("enabled"));
    EXTENDED = own(BooleanProperty::create("extended"));
    TRIGGERED = own(BooleanProperty::create("triggered"));

    // Directional connection properties
    // Reference: BlockStateProperties.java lines 155-160
    UP = own(BooleanProperty::create("up"));
    DOWN = own(BooleanProperty::create("down"));
    NORTH = own(BooleanProperty::create("north"));
    EAST = own(BooleanProperty::create("east"));
    SOUTH = own(BooleanProperty::create("south"));
    WEST = own(BooleanProperty::create("west"));

    // Direction properties
    // Reference: BlockStateProperties.java lines 161-163
    FACING = own(DirectionProperty::create("facing"));
    HORIZONTAL_FACING = own(DirectionProperty::createHorizontal("facing"));
    VERTICAL_DIRECTION = own(DirectionProperty::create("vertical_direction",
        { core::Direction::UP, core::Direction::DOWN }));

    // Axis properties
    // Reference: BlockStateProperties.java lines 153-154
    AXIS = own(AxisProperty::create("axis"));
    HORIZONTAL_AXIS = own(AxisProperty::createHorizontal("axis"));

    // Age properties
    // Reference: BlockStateProperties.java lines 182-189
    AGE_1 = own(IntegerProperty::create("age", 0, 1));
    AGE_2 = own(IntegerProperty::create("age", 0, 2));
    AGE_3 = own(IntegerProperty::create("age", 0, 3));
    AGE_4 = own(IntegerProperty::create("age", 0, 4));
    AGE_5 = own(IntegerProperty::create("age", 0, 5));
    AGE_7 = own(IntegerProperty::create("age", 0, 7));
    AGE_15 = own(IntegerProperty::create("age", 0, 15));
    AGE_25 = own(IntegerProperty::create("age", 0, 25));

    // Other integer properties
    // Reference: BlockStateProperties.java lines 193-210
    DISTANCE = own(IntegerProperty::create("distance", 1, 7));
    LEVEL = own(IntegerProperty::create("level", 0, 15));
    LEVEL_CAULDRON = own(IntegerProperty::create("level", 1, 3));
    LEVEL_COMPOSTER = own(IntegerProperty::create("level", 0, 8));
    POWER = own(IntegerProperty::create("power", 0, 15));
    MOISTURE = own(IntegerProperty::create("moisture", 0, 7));
    LAYERS = own(IntegerProperty::create("layers", 1, 8));
    ROTATION_16 = own(IntegerProperty::create("rotation", 0, 15));
    NOTE = own(IntegerProperty::create("note", 0, 24));
    STAGE = own(IntegerProperty::create("stage", 0, 1));

    // Enum properties
    // Reference: BlockStateProperties.java lines 178, 217-218
    HALF = own(EnumProperty<Half>::create("half", Half::values()));
    SLAB_TYPE = own(EnumProperty<SlabType>::create("type", SlabType::values()));
    STAIRS_SHAPE = own(EnumProperty<StairsShape>::create("shape", StairsShape::values()));
    ATTACH_FACE = own(EnumProperty<AttachFace>::create("face", AttachFace::values()));
    DOUBLE_BLOCK_HALF = own(EnumProperty<DoubleBlockHalf>::create("half", DoubleBlockHalf::values()));
    BED_PART = own(EnumProperty<BedPart>::create("part", BedPart::values()));
    CHEST_TYPE = own(EnumProperty<ChestType>::create("type", ChestType::values()));
    DOOR_HINGE = own(EnumProperty<DoorHingeSide>::create("hinge", DoorHingeSide::values()));
    PISTON_TYPE = own(EnumProperty<PistonType>::create("type", PistonType::values()));
    MODE_COMPARATOR = own(EnumProperty<ComparatorMode>::create("mode", ComparatorMode::values()));
    BAMBOO_LEAVES = own(EnumProperty<BambooLeaves>::create("leaves", BambooLeaves::values()));
    TILT = own(EnumProperty<Tilt>::create("tilt", Tilt::values()));
    DRIPSTONE_THICKNESS = own(EnumProperty<DripstoneThickness>::create("thickness", DripstoneThickness::values()));
    SCULK_SENSOR_PHASE = own(EnumProperty<SculkSensorPhase>::create("sculk_sensor_phase", SculkSensorPhase::values()));
    STRUCTUREBLOCK_MODE = own(EnumProperty<StructureMode>::create("mode", StructureMode::values()));
    BELL_ATTACHMENT = own(EnumProperty<BellAttachType>::create("attachment", BellAttachType::values()));
    RAIL_SHAPE = own(EnumProperty<RailShape>::create("shape", RailShape::values()));

    // Rail shape straight - filter out curved shapes
    std::vector<RailShape> straightShapes;
    for (const auto& shape : RailShape::values()) {
        if (shape.getValue() != RailShape::NORTH_EAST && shape.getValue() != RailShape::NORTH_WEST &&
            shape.getValue() != RailShape::SOUTH_EAST && shape.getValue() != RailShape::SOUTH_WEST) {
            straightShapes.push_back(shape);
        }
    }
    RAIL_SHAPE_STRAIGHT = own(EnumProperty<RailShape>::create("shape", straightShapes));

    // Wall connection properties
    EAST_WALL = own(EnumProperty<WallSide>::create("east", WallSide::values()));
    NORTH_WALL = own(EnumProperty<WallSide>::create("north", WallSide::values()));
    SOUTH_WALL = own(EnumProperty<WallSide>::create("south", WallSide::values()));
    WEST_WALL = own(EnumProperty<WallSide>::create("west", WallSide::values()));

    // Redstone connection properties
    EAST_REDSTONE = own(EnumProperty<RedstoneSide>::create("east", RedstoneSide::values()));
    NORTH_REDSTONE = own(EnumProperty<RedstoneSide>::create("north", RedstoneSide::values()));
    SOUTH_REDSTONE = own(EnumProperty<RedstoneSide>::create("south", RedstoneSide::values()));
    WEST_REDSTONE = own(EnumProperty<RedstoneSide>::create("west", RedstoneSide::values()));

    // Additional integer properties
    // Reference: BlockStateProperties.java lines 190-209
    BITES = own(IntegerProperty::create("bites", 0, 6));
    CANDLES = own(IntegerProperty::create("candles", 1, 4));
    DELAY = own(IntegerProperty::create("delay", 1, 4));
    EGGS = own(IntegerProperty::create("eggs", 1, 4));
    HATCH = own(IntegerProperty::create("hatch", 0, 2));
    LEVEL_FLOWING = own(IntegerProperty::create("level", 1, 8));
    LEVEL_HONEY = own(IntegerProperty::create("honey_level", 0, 5));
    PICKLES = own(IntegerProperty::create("pickles", 1, 4));
    STABILITY_DISTANCE = own(IntegerProperty::create("distance", 0, 7));
    RESPAWN_ANCHOR_CHARGES = own(IntegerProperty::create("charges", 0, 4));
    DUSTED = own(IntegerProperty::create("dusted", 0, 3));
    FLOWER_AMOUNT = own(IntegerProperty::create("flower_amount", 1, 4));
    SEGMENT_AMOUNT = own(IntegerProperty::create("segment_amount", 1, 4));

    // Additional boolean properties
    // Reference: BlockStateProperties.java lines 11-45
    BOTTOM = own(BooleanProperty::create("bottom"));
    CONDITIONAL = own(BooleanProperty::create("conditional"));
    DISARMED = own(BooleanProperty::create("disarmed"));
    DRAG = own(BooleanProperty::create("drag"));
    EYE = own(BooleanProperty::create("eye"));
    FALLING = own(BooleanProperty::create("falling"));
    INVERTED = own(BooleanProperty::create("inverted"));
    IN_WALL = own(BooleanProperty::create("in_wall"));
    LOCKED = own(BooleanProperty::create("locked"));
    NATURAL = own(BooleanProperty::create("natural"));
    SHORT_PISTON = own(BooleanProperty::create("short"));
    SHRIEKING = own(BooleanProperty::create("shrieking"));
    SIGNAL_FIRE = own(BooleanProperty::create("signal_fire"));
    TIP = own(BooleanProperty::create("tip"));
    UNSTABLE = own(BooleanProperty::create("unstable"));
    BERRIES = own(BooleanProperty::create("berries"));
    BLOOM = own(BooleanProperty::create("bloom"));
    CAN_SUMMON = own(BooleanProperty::create("can_summon"));
    HAS_BOTTLE_0 = own(BooleanProperty::create("has_bottle_0"));
    HAS_BOTTLE_1 = own(BooleanProperty::create("has_bottle_1"));
    HAS_BOTTLE_2 = own(BooleanProperty::create("has_bottle_2"));
    HAS_RECORD = own(BooleanProperty::create("has_record"));
    HAS_BOOK = own(BooleanProperty::create("has_book"));
    CRACKED = own(BooleanProperty::create("cracked"));
    CRAFTING = own(BooleanProperty::create("crafting"));
    OMINOUS = own(BooleanProperty::create("ominous"));

    s_initialized = true;
}

bool BlockStateProperties::isInitialized() {
    return s_initialized;
}

} // namespace properties
} // namespace state
} // namespace block
} // namespace level
} // namespace world
} // namespace minecraft
