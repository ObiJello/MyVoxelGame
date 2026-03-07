#include "world/level/block/Blocks.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

namespace minecraft {
namespace world {
namespace level {
namespace block {

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
Block* minecraft::world::level::block::Blocks::GLOW_LICHEN = nullptr;
Block* minecraft::world::level::block::Blocks::AZALEA = nullptr;
Block* minecraft::world::level::block::Blocks::FLOWERING_AZALEA = nullptr;
Block* minecraft::world::level::block::Blocks::SPORE_BLOSSOM = nullptr;
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

// Other vegetation
Block* minecraft::world::level::block::Blocks::CACTUS = nullptr;
Block* minecraft::world::level::block::Blocks::SUGAR_CANE = nullptr;
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
    s_blocksByName[name] = block;
}

Block* minecraft::world::level::block::Blocks::createSimpleBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new Block(props);
    registerBlock(name, block);
    return block;
}

Block* minecraft::world::level::block::Blocks::createAirBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).air();
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
    props.setId(name).noCollission().replaceable();
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

BushBlock* minecraft::world::level::block::Blocks::createBushBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new BushBlock(props);
    registerBlock(name, block);
    return block;
}

VineBlock* minecraft::world::level::block::Blocks::createVineBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new VineBlock(props);
    registerBlock(name, block);
    return block;
}

AzaleaBlock* minecraft::world::level::block::Blocks::createAzaleaBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new AzaleaBlock(props);
    registerBlock(name, block);
    return block;
}

CaveVinesBlock* minecraft::world::level::block::Blocks::createCaveVinesBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new CaveVinesBlock(props);
    registerBlock(name, block);
    return block;
}

CaveVinesPlantBlock* minecraft::world::level::block::Blocks::createCaveVinesPlantBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new CaveVinesPlantBlock(props);
    registerBlock(name, block);
    return block;
}

SmallDripleafBlock* minecraft::world::level::block::Blocks::createSmallDripleafBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new SmallDripleafBlock(props);
    registerBlock(name, block);
    return block;
}

BigDripleafBlock* minecraft::world::level::block::Blocks::createBigDripleafBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
    auto block = new BigDripleafBlock(props);
    registerBlock(name, block);
    return block;
}

BigDripleafStemBlock* minecraft::world::level::block::Blocks::createBigDripleafStemBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name);
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
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new TallFlowerBlock(props);
    registerBlock(name, block);
    return block;
}

FlowerBedBlock* minecraft::world::level::block::Blocks::createFlowerBedBlock(const std::string& name) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
    auto block = new FlowerBedBlock(props);
    registerBlock(name, block);
    return block;
}

EyeblossomBlock* minecraft::world::level::block::Blocks::createEyeblossomBlock(const std::string& name, bool open) {
    Block::Properties props;
    props.setId(name).replaceable().replaceableByTrees();
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
    // TEMPORARY: Using simple Block to avoid state system issues
    // TODO: Fix std::any_cast issue in StateDefinition and use LeavesBlock
    Block::Properties props;
    props.setId(name).leaves().replaceableByTrees();
    auto block = new Block(props);
    registerBlock(name, block);
    return reinterpret_cast<LeavesBlock*>(block);
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
    DEEPSLATE = createSimpleBlock("minecraft:deepslate");
    COBBLESTONE = createSimpleBlock("minecraft:cobblestone");
    MOSSY_COBBLESTONE = createSimpleBlock("minecraft:mossy_cobblestone");
    DIRT = createSimpleBlock("minecraft:dirt");
    ROOTED_DIRT = createSimpleBlock("minecraft:rooted_dirt");
    COARSE_DIRT = createSimpleBlock("minecraft:coarse_dirt");
    PODZOL = createSimpleBlock("minecraft:podzol");
    GRASS_BLOCK = createSimpleBlock("minecraft:grass_block");
    SAND = createSimpleBlock("minecraft:sand");
    GRAVEL = createSimpleBlock("minecraft:gravel");
    BEDROCK = createSimpleBlock("minecraft:bedrock");
    TUFF = createSimpleBlock("minecraft:tuff");
    DRIPSTONE_BLOCK = createSimpleBlock("minecraft:dripstone_block");
    POINTED_DRIPSTONE = createPlantBlock("minecraft:pointed_dripstone");
    SANDSTONE = createSimpleBlock("minecraft:sandstone");

    // =========================================================================
    // Liquids
    // =========================================================================
    {
        Block::Properties props;
        props.setId("minecraft:water").liquid().noCollission().replaceableByTrees();
        WATER = new Block(props);
        registerBlock("minecraft:water", WATER);
    }
    LAVA = createLiquidBlock("minecraft:lava");

    // =========================================================================
    // Ice and snow
    // =========================================================================
    SNOW_BLOCK = createSimpleBlock("minecraft:snow_block");
    PACKED_ICE = createSimpleBlock("minecraft:packed_ice");
    BLUE_ICE = createSimpleBlock("minecraft:blue_ice");
    ICE = createSimpleBlock("minecraft:ice");
    {
        Block::Properties props;
        props.setId("minecraft:powder_snow").noCollission().replaceable();
        POWDER_SNOW = new Block(props);
        registerBlock("minecraft:powder_snow", POWDER_SNOW);
    }
    // Snow layer block (1-8 layers)
    {
        Block::Properties props;
        props.setId("minecraft:snow").noCollission().replaceable();
        SNOW = new Block(props);
        registerBlock("minecraft:snow", SNOW);
    }

    // =========================================================================
    // Geode blocks
    // Reference: Used in amethyst geode feature
    // =========================================================================
    AMETHYST_BLOCK = createSimpleBlock("minecraft:amethyst_block");
    BUDDING_AMETHYST = createSimpleBlock("minecraft:budding_amethyst");
    CALCITE = createSimpleBlock("minecraft:calcite");
    SMOOTH_BASALT = createSimpleBlock("minecraft:smooth_basalt");
    SMALL_AMETHYST_BUD = createPlantBlock("minecraft:small_amethyst_bud");
    MEDIUM_AMETHYST_BUD = createPlantBlock("minecraft:medium_amethyst_bud");
    LARGE_AMETHYST_BUD = createPlantBlock("minecraft:large_amethyst_bud");
    AMETHYST_CLUSTER = createPlantBlock("minecraft:amethyst_cluster");

    // =========================================================================
    // Clay and mud blocks
    // =========================================================================
    CLAY = createSimpleBlock("minecraft:clay");
    MUD = createSimpleBlock("minecraft:mud");
    MUDDY_MANGROVE_ROOTS = createSimpleBlock("minecraft:muddy_mangrove_roots");
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
    SCULK_CATALYST = createSimpleBlock("minecraft:sculk_catalyst");
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
    REDSTONE_ORE = createSimpleBlock("minecraft:redstone_ore");
    DEEPSLATE_REDSTONE_ORE = createSimpleBlock("minecraft:deepslate_redstone_ore");
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
    INFESTED_DEEPSLATE = createSimpleBlock("minecraft:infested_deepslate");

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
    DEAD_BUSH = createBushBlock("minecraft:dead_bush");
    BUSH = createBushBlock("minecraft:bush");

    // =========================================================================
    // Flowers (no collision)
    // =========================================================================
    DANDELION = createBushBlock("minecraft:dandelion");
    POPPY = createBushBlock("minecraft:poppy");
    BLUE_ORCHID = createBushBlock("minecraft:blue_orchid");
    ALLIUM = createBushBlock("minecraft:allium");
    AZURE_BLUET = createBushBlock("minecraft:azure_bluet");
    RED_TULIP = createBushBlock("minecraft:red_tulip");
    ORANGE_TULIP = createBushBlock("minecraft:orange_tulip");
    WHITE_TULIP = createBushBlock("minecraft:white_tulip");
    PINK_TULIP = createBushBlock("minecraft:pink_tulip");
    OXEYE_DAISY = createBushBlock("minecraft:oxeye_daisy");
    CORNFLOWER = createBushBlock("minecraft:cornflower");
    LILY_OF_THE_VALLEY = createBushBlock("minecraft:lily_of_the_valley");

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
    BROWN_MUSHROOM = createBushBlock("minecraft:brown_mushroom");
    RED_MUSHROOM = createBushBlock("minecraft:red_mushroom");

    // =========================================================================
    // Huge mushroom blocks (solid)
    // Note: These have directional properties in full implementation
    // =========================================================================
    BROWN_MUSHROOM_BLOCK = createSimpleBlock("minecraft:brown_mushroom_block");
    RED_MUSHROOM_BLOCK = createSimpleBlock("minecraft:red_mushroom_block");
    MUSHROOM_STEM = createSimpleBlock("minecraft:mushroom_stem");

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
    GLOW_LICHEN = createReplaceableByTreesBlock("minecraft:glow_lichen");
    AZALEA = createAzaleaBlock("minecraft:azalea");
    FLOWERING_AZALEA = createAzaleaBlock("minecraft:flowering_azalea");
    SPORE_BLOSSOM = createReplaceablePlantBlock("minecraft:spore_blossom");
    BIG_DRIPLEAF = createBigDripleafBlock("minecraft:big_dripleaf");
    BIG_DRIPLEAF_STEM = createBigDripleafStemBlock("minecraft:big_dripleaf_stem");
    SMALL_DRIPLEAF = createSmallDripleafBlock("minecraft:small_dripleaf");

    // =========================================================================
    // Pale garden vegetation
    // Reference: Used in pale garden biome features
    // =========================================================================
    PALE_MOSS_BLOCK = createSimpleBlock("minecraft:pale_moss_block");
    PALE_MOSS_CARPET = createReplaceableByTreesBlock("minecraft:pale_moss_carpet");
    PALE_HANGING_MOSS = createReplaceablePlantBlock("minecraft:pale_hanging_moss");
    CLOSED_EYEBLOSSOM = createEyeblossomBlock("minecraft:closed_eyeblossom", false);

    // =========================================================================
    // Ocean vegetation
    // Reference: Used in ocean biome features
    // =========================================================================
    SEAGRASS = createReplaceableByTreesBlock("minecraft:seagrass");
    TALL_SEAGRASS = createReplaceableByTreesBlock("minecraft:tall_seagrass");
    KELP = createReplaceablePlantBlock("minecraft:kelp");
    KELP_PLANT = createReplaceablePlantBlock("minecraft:kelp_plant");

    // =========================================================================
    // Other vegetation
    // =========================================================================
    CACTUS = createSimpleBlock("minecraft:cactus");
    SUGAR_CANE = createReplaceablePlantBlock("minecraft:sugar_cane");
    PUMPKIN = createSimpleBlock("minecraft:pumpkin");
    MELON = createSimpleBlock("minecraft:melon");
    COCOA = createReplaceablePlantBlock("minecraft:cocoa");
    MANGROVE_ROOTS = createSimpleBlock("minecraft:mangrove_roots");
    OAK_SAPLING = createReplaceablePlantBlock("minecraft:oak_sapling");
    SPRUCE_SAPLING = createReplaceablePlantBlock("minecraft:spruce_sapling");
    BIRCH_SAPLING = createReplaceablePlantBlock("minecraft:birch_sapling");
    JUNGLE_SAPLING = createReplaceablePlantBlock("minecraft:jungle_sapling");
    ACACIA_SAPLING = createReplaceablePlantBlock("minecraft:acacia_sapling");
    CHERRY_SAPLING = createReplaceablePlantBlock("minecraft:cherry_sapling");
    DARK_OAK_SAPLING = createReplaceablePlantBlock("minecraft:dark_oak_sapling");
    PALE_OAK_SAPLING = createReplaceablePlantBlock("minecraft:pale_oak_sapling");
    MANGROVE_PROPAGULE = createReplaceablePlantBlock("minecraft:mangrove_propagule");

    // =========================================================================
    // Dungeon blocks
    // =========================================================================
    SPAWNER = createSimpleBlock("minecraft:spawner");
    CHEST = createSimpleBlock("minecraft:chest");
    BEE_NEST = createSimpleBlock("minecraft:bee_nest");

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
