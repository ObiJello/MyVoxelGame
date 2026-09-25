#define TILE_CONSTANTS_FireTile \
    static constexpr int FLAME_INSTANT = 60; \
    static constexpr int FLAME_EASY = 30; \
    static constexpr int FLAME_MEDIUM = 15; \
    static constexpr int FLAME_HARD = 5; \
    static constexpr int BURN_INSTANT = 100; \
    static constexpr int BURN_EASY = 60; \
    static constexpr int BURN_MEDIUM = 20; \
    static constexpr int BURN_HARD = 5; \
    static constexpr int BURN_NEVER = 0;
#define TILE_CONSTANTS_HeavyTile \
    
#define TILE_CONSTANTS_TopSnowTile \
    static constexpr int MAX_HEIGHT = 6; \
    static constexpr int HEIGHT_MASK = 7;
#define TILE_CONSTANTS_DoorTile \
    static constexpr int UPPER_BIT = 8; \
    static constexpr int C_DIR_MASK = 3; \
    static constexpr int C_OPEN_MASK = 4; \
    static constexpr int C_LOWER_DATA_MASK = 7; \
    static constexpr int C_IS_UPPER_MASK = 8; \
    static constexpr int C_RIGHT_HINGE_MASK = 16; \
    static constexpr int DOOR_TILE_TEXTURE_COUNT = 4;
#define TILE_CONSTANTS_TntTile \
    static constexpr int EXPLODE_BIT = 1;
#define TILE_CONSTANTS_StairTile \
    static constexpr int DEAD_SPACE_COLUMN_COUNT = 2; \
    static constexpr int UPSIDEDOWN_BIT = 4; \
    static constexpr int DIR_EAST = 0; \
    static constexpr int DIR_WEST = 1; \
    static constexpr int DIR_SOUTH = 2; \
    static constexpr int DIR_NORTH = 3;
#define TILE_CONSTANTS_HalfSlabTile \
    static constexpr int TYPE_MASK = 7; \
    static constexpr int TOP_SLOT_BIT = 8;
#define TILE_CONSTANTS_NotGateTile \
    static constexpr int RECENT_TOGGLE_TIMER = 20 * 3; \
    static constexpr int MAX_RECENT_TOGGLES = 8;
#define TILE_CONSTANTS_DiodeTile \
    static constexpr int DELAY_MASK = DIRECTION_INV_MASK; \
    static constexpr int DELAY_SHIFT = 2;
#define TILE_CONSTANTS_TrapDoorTile \
    static constexpr int TOP_MASK = 0x8;
#define TILE_CONSTANTS_FenceGateTile \
    static constexpr int OPEN_BIT = 4;
#define TILE_CONSTANTS_PistonBaseTile \
    static constexpr int EXTENDED_BIT = 8; \
    static constexpr int UNDEFINED_FACING = 7; \
    static constexpr int MAX_PUSH_DEPTH = 12; \
    static constexpr int TRIGGER_EXTEND = 0; \
    static constexpr int TRIGGER_CONTRACT = 1;
#define TILE_CONSTANTS_PistonExtensionTile \
    static constexpr int STICKY_BIT = 8;
#define TILE_CONSTANTS_DispenserTile \
    static constexpr int DISPENSE_ITEM = 0; \
    static constexpr int REMOVE_ITEM = 1; \
    static constexpr int LEAVE_ITEM = 2; \
    static constexpr int FACING_MASK = 0x7;
#define TILE_CONSTANTS_MobCategory \
    static constexpr int CONSOLE_MONSTERS_HARD_LIMIT = 50; \
    static constexpr int CONSOLE_ANIMALS_HARD_LIMIT = 50; \
    static constexpr int MAX_XBOX_CHICKENS = 8; \
    static constexpr int MAX_XBOX_WOLVES = 8; \
    static constexpr int MAX_XBOX_MUSHROOMCOWS = 2; \
    static constexpr int MAX_XBOX_SNOWMEN = 16; \
    static constexpr int MAX_XBOX_IRONGOLEM = 16; \
    static constexpr int CONSOLE_SQUID_HARD_LIMIT = 5; \
    static constexpr int MAX_XBOX_ANIMALS_WITH_BREEDING = CONSOLE_ANIMALS_HARD_LIMIT + 20; \
    static constexpr int MAX_XBOX_CHICKENS_WITH_BREEDING = MAX_XBOX_CHICKENS + 8; \
    static constexpr int MAX_XBOX_MUSHROOMCOWS_WITH_BREEDING = MAX_XBOX_MUSHROOMCOWS + 20; \
    static constexpr int MAX_XBOX_WOLVES_WITH_BREEDING = MAX_XBOX_WOLVES + 8; \
    static constexpr int MAX_VILLAGERS_WITH_BREEDING = 35; \
    static constexpr int MAX_XBOX_ANIMALS_WITH_SPAWN_EGG = MAX_XBOX_ANIMALS_WITH_BREEDING + 20; \
    static constexpr int MAX_XBOX_CHICKENS_WITH_SPAWN_EGG = MAX_XBOX_CHICKENS_WITH_BREEDING + 10; \
    static constexpr int MAX_XBOX_WOLVES_WITH_SPAWN_EGG = MAX_XBOX_WOLVES_WITH_BREEDING + 10; \
    static constexpr int MAX_XBOX_MONSTERS_WITH_SPAWN_EGG = CONSOLE_MONSTERS_HARD_LIMIT + 20; \
    static constexpr int MAX_XBOX_VILLAGERS_WITH_SPAWN_EGG = MAX_VILLAGERS_WITH_BREEDING + 15; \
    static constexpr int MAX_XBOX_MUSHROOMCOWS_WITH_SPAWN_EGG = MAX_XBOX_MUSHROOMCOWS_WITH_BREEDING + 8; \
    static constexpr int MAX_XBOX_SQUIDS_WITH_SPAWN_EGG = CONSOLE_SQUID_HARD_LIMIT + 8;
#define TILE_CONSTANTS_DyePowderItem \
    static constexpr int BLACK = 0; \
    static constexpr int RED = 1; \
    static constexpr int GREEN = 2; \
    static constexpr int BROWN = 3; \
    static constexpr int BLUE = 4; \
    static constexpr int PURPLE = 5; \
    static constexpr int CYAN = 6; \
    static constexpr int SILVER = 7; \
    static constexpr int GRAY = 8; \
    static constexpr int PINK = 9; \
    static constexpr int LIME = 10; \
    static constexpr int YELLOW = 11; \
    static constexpr int LIGHT_BLUE = 12; \
    static constexpr int MAGENTA = 13; \
    static constexpr int ORANGE = 14; \
    static constexpr int WHITE = 15; \
    static constexpr int DYE_POWDER_ITEM_TEXTURE_COUNT = 16;
