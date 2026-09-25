#define TILE_CONSTANTS_FireTile \
    static const int FLAME_INSTANT = 60; \
    static const int FLAME_EASY = 30; \
    static const int FLAME_MEDIUM = 15; \
    static const int FLAME_HARD = 5; \
    static const int BURN_INSTANT = 100; \
    static const int BURN_EASY = 60; \
    static const int BURN_MEDIUM = 20; \
    static const int BURN_HARD = 5; \
    static const int BURN_NEVER = 0;
#define TILE_CONSTANTS_HeavyTile \
    
#define TILE_CONSTANTS_TopSnowTile \
    static const int MAX_HEIGHT = 6; \
    static const int HEIGHT_MASK = 7;
#define TILE_CONSTANTS_DoorTile \
    static const int UPPER_BIT = 8; \
    static const int C_DIR_MASK = 3; \
    static const int C_OPEN_MASK = 4; \
    static const int C_LOWER_DATA_MASK = 7; \
    static const int C_IS_UPPER_MASK = 8; \
    static const int C_RIGHT_HINGE_MASK = 16; \
    static const int DOOR_TILE_TEXTURE_COUNT = 4;
#define TILE_CONSTANTS_TntTile \
    static const int EXPLODE_BIT = 1;
#define TILE_CONSTANTS_StairTile \
    static const int DEAD_SPACE_COLUMN_COUNT = 2; \
    static const int UPSIDEDOWN_BIT = 4; \
    static const int DIR_EAST = 0; \
    static const int DIR_WEST = 1; \
    static const int DIR_SOUTH = 2; \
    static const int DIR_NORTH = 3;
#define TILE_CONSTANTS_HalfSlabTile \
    static const int TYPE_MASK = 7; \
    static const int TOP_SLOT_BIT = 8;
#define TILE_CONSTANTS_NotGateTile \
    static const int RECENT_TOGGLE_TIMER = 20 * 3; \
    static const int MAX_RECENT_TOGGLES = 8;
#define TILE_CONSTANTS_DiodeTile \
    static const int DELAY_MASK = DIRECTION_INV_MASK; \
    static const int DELAY_SHIFT = 2;
#define TILE_CONSTANTS_TrapDoorTile \
    static const int TOP_MASK = 0x8;
#define TILE_CONSTANTS_FenceGateTile \
    static const int OPEN_BIT = 4;
#define TILE_CONSTANTS_PistonBaseTile \
    static const int EXTENDED_BIT = 8; \
    static const int UNDEFINED_FACING = 7; \
    static const int MAX_PUSH_DEPTH = 12; \
    static const int TRIGGER_EXTEND = 0; \
    static const int TRIGGER_CONTRACT = 1;
#define TILE_CONSTANTS_PistonExtensionTile \
    static const int STICKY_BIT = 8;
