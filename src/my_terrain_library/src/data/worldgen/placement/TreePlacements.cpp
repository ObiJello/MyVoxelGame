#include "data/worldgen/placement/TreePlacements.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/Blocks.h"
#include <deque>

// Reference: net/minecraft/data/worldgen/placement/TreePlacements.java
// CRITICAL: Implement EXACTLY as Java does for 100% bit parity

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace ::minecraft::levelgen::placement;
using namespace ::minecraft::data::worldgen::features;
using ::minecraft::levelgen::Heightmap;

// Static members
bool TreePlacements::s_initialized = false;

// =========================================================================
// STATIC MEMBER DEFINITIONS - All PlacedFeature pointers initialized to nullptr
// Reference: TreePlacements.java lines 22-62
// =========================================================================

// Nether Fungi
const PlacedFeature* TreePlacements::CRIMSON_FUNGI = nullptr;
const PlacedFeature* TreePlacements::WARPED_FUNGI = nullptr;

// Basic checked trees
const PlacedFeature* TreePlacements::OAK_CHECKED = nullptr;
const PlacedFeature* TreePlacements::DARK_OAK_CHECKED = nullptr;
const PlacedFeature* TreePlacements::PALE_OAK_CHECKED = nullptr;
const PlacedFeature* TreePlacements::PALE_OAK_CREAKING_CHECKED = nullptr;
const PlacedFeature* TreePlacements::BIRCH_CHECKED = nullptr;
const PlacedFeature* TreePlacements::ACACIA_CHECKED = nullptr;
const PlacedFeature* TreePlacements::SPRUCE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::MANGROVE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::CHERRY_CHECKED = nullptr;
const PlacedFeature* TreePlacements::PINE_ON_SNOW = nullptr;
const PlacedFeature* TreePlacements::SPRUCE_ON_SNOW = nullptr;
const PlacedFeature* TreePlacements::PINE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::JUNGLE_TREE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::FANCY_OAK_CHECKED = nullptr;
const PlacedFeature* TreePlacements::MEGA_JUNGLE_TREE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::MEGA_SPRUCE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::MEGA_PINE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::TALL_MANGROVE_CHECKED = nullptr;
const PlacedFeature* TreePlacements::JUNGLE_BUSH = nullptr;

// Birch with bees
const PlacedFeature* TreePlacements::SUPER_BIRCH_BEES_0002 = nullptr;
const PlacedFeature* TreePlacements::SUPER_BIRCH_BEES = nullptr;

// Oak with bees
const PlacedFeature* TreePlacements::OAK_BEES_0002_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::OAK_BEES_002 = nullptr;

// Birch with bees (regular)
const PlacedFeature* TreePlacements::BIRCH_BEES_0002_PLACED = nullptr;
const PlacedFeature* TreePlacements::BIRCH_BEES_0002_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::BIRCH_BEES_002 = nullptr;

// Fancy oak with bees
const PlacedFeature* TreePlacements::FANCY_OAK_BEES_0002_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::FANCY_OAK_BEES_002 = nullptr;
const PlacedFeature* TreePlacements::FANCY_OAK_BEES = nullptr;

// Cherry with bees
const PlacedFeature* TreePlacements::CHERRY_BEES_005 = nullptr;

// Leaf litter variants
const PlacedFeature* TreePlacements::OAK_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::DARK_OAK_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::BIRCH_LEAF_LITTER = nullptr;
const PlacedFeature* TreePlacements::FANCY_OAK_LEAF_LITTER = nullptr;

// Fallen trees
const PlacedFeature* TreePlacements::FALLEN_OAK_TREE = nullptr;
const PlacedFeature* TreePlacements::FALLEN_BIRCH_TREE = nullptr;
const PlacedFeature* TreePlacements::FALLEN_SUPER_BIRCH_TREE = nullptr;
const PlacedFeature* TreePlacements::FALLEN_SPRUCE_TREE = nullptr;
const PlacedFeature* TreePlacements::FALLEN_JUNGLE_TREE = nullptr;

// =========================================================================
// STORAGE - Use deque to avoid pointer invalidation
// =========================================================================
static std::deque<PlacedFeature> s_placedFeatures;
static std::deque<CountOnEveryLayerPlacement> s_countOnEveryLayerPlacements;
static std::deque<EnvironmentScanPlacement> s_environmentScanPlacements;
static std::deque<BlockPredicateFilter> s_blockPredicateFilters;

// =========================================================================
// HELPER METHODS
// =========================================================================

std::vector<PlacementModifier*> TreePlacements::filteredByBlockSurvival(BlockState* state) {
    // Reference: PlacementUtils.filteredByBlockSurvival(Block)
    auto wouldSurvive = levelgen::blockpredicates::BlockPredicate::wouldSurvive(state, core::Vec3i::ZERO());
    s_blockPredicateFilters.push_back(BlockPredicateFilter::forPredicate(wouldSurvive));
    return { &s_blockPredicateFilters.back() };
}

// =========================================================================
// BOOTSTRAP - Initialize all PlacedFeatures exactly as Java does
// Reference: TreePlacements.java bootstrap() lines 64-148
// =========================================================================

void TreePlacements::bootstrap() {
    if (s_initialized) return;

    // Ensure tree features are bootstrapped first
    if (!TreeFeatures::isInitialized()) {
        TreeFeatures::bootstrap();
    }

    // Helper to create PlacedFeature and store it
    auto createPlaced = [](levelgen::ConfiguredFeature* feature, std::vector<PlacementModifier*> modifiers, const std::string& name = "") -> const PlacedFeature* {
        if (feature == nullptr) {
            return nullptr;  // Skip if feature doesn't exist
        }
        s_placedFeatures.emplace_back(feature, modifiers, name);
        return &s_placedFeatures.back();
    };

    BlockState* oakSapling = world::level::block::Blocks::getDefaultState("minecraft:oak_sapling");
    BlockState* spruceSapling = world::level::block::Blocks::getDefaultState("minecraft:spruce_sapling");
    BlockState* birchSapling = world::level::block::Blocks::getDefaultState("minecraft:birch_sapling");
    BlockState* jungleSapling = world::level::block::Blocks::getDefaultState("minecraft:jungle_sapling");
    BlockState* acaciaSapling = world::level::block::Blocks::getDefaultState("minecraft:acacia_sapling");
    BlockState* cherrySapling = world::level::block::Blocks::getDefaultState("minecraft:cherry_sapling");
    BlockState* darkOakSapling = world::level::block::Blocks::getDefaultState("minecraft:dark_oak_sapling");
    BlockState* paleOakSapling = world::level::block::Blocks::getDefaultState("minecraft:pale_oak_sapling");
    BlockState* mangrovePropagule = world::level::block::Blocks::getDefaultState("minecraft:mangrove_propagule");

    // =========================================================================
    // NETHER FUNGI - Reference: Java lines 105-106
    // PlacementUtils.register(context, CRIMSON_FUNGI, crimsonFungus, CountOnEveryLayerPlacement.of(8), BiomeFilter.biome());
    // =========================================================================

    s_countOnEveryLayerPlacements.push_back(CountOnEveryLayerPlacement::of(8));
    CRIMSON_FUNGI = createPlaced(
        TreeFeatures::CRIMSON_FUNGUS,
        { &s_countOnEveryLayerPlacements.back(), &BiomeFilter::biome() },
        "CRIMSON_FUNGI"
    );

    s_countOnEveryLayerPlacements.push_back(CountOnEveryLayerPlacement::of(8));
    WARPED_FUNGI = createPlaced(
        TreeFeatures::WARPED_FUNGUS,
        { &s_countOnEveryLayerPlacements.back(), &BiomeFilter::biome() },
        "WARPED_FUNGI"
    );

    // =========================================================================
    // BASIC CHECKED TREES - Reference: Java lines 107-127
    // All use PlacementUtils.filteredByBlockSurvival(sapling)
    // =========================================================================

    // OAK_CHECKED
    OAK_CHECKED = createPlaced(TreeFeatures::OAK, filteredByBlockSurvival(oakSapling), "OAK_CHECKED");

    // DARK_OAK_CHECKED
    DARK_OAK_CHECKED = createPlaced(TreeFeatures::DARK_OAK, filteredByBlockSurvival(darkOakSapling), "DARK_OAK_CHECKED");

    // PALE_OAK_CHECKED
    PALE_OAK_CHECKED = createPlaced(TreeFeatures::PALE_OAK, filteredByBlockSurvival(paleOakSapling), "PALE_OAK_CHECKED");

    // PALE_OAK_CREAKING_CHECKED
    PALE_OAK_CREAKING_CHECKED = createPlaced(TreeFeatures::PALE_OAK_CREAKING, filteredByBlockSurvival(paleOakSapling), "PALE_OAK_CREAKING_CHECKED");

    // BIRCH_CHECKED
    BIRCH_CHECKED = createPlaced(TreeFeatures::BIRCH, filteredByBlockSurvival(birchSapling), "BIRCH_CHECKED");

    // ACACIA_CHECKED
    ACACIA_CHECKED = createPlaced(TreeFeatures::ACACIA, filteredByBlockSurvival(acaciaSapling), "ACACIA_CHECKED");

    // SPRUCE_CHECKED
    SPRUCE_CHECKED = createPlaced(TreeFeatures::SPRUCE, filteredByBlockSurvival(spruceSapling), "SPRUCE_CHECKED");

    // MANGROVE_CHECKED
    MANGROVE_CHECKED = createPlaced(TreeFeatures::MANGROVE, filteredByBlockSurvival(mangrovePropagule), "MANGROVE_CHECKED");

    // CHERRY_CHECKED
    CHERRY_CHECKED = createPlaced(TreeFeatures::CHERRY, filteredByBlockSurvival(cherrySapling), "CHERRY_CHECKED");

    // =========================================================================
    // SNOW TREES - Reference: Java lines 116-119
    // These use special snow predicates:
    // BlockPredicate snowTreePredicate = BlockPredicate.matchesBlocks(Direction.DOWN.getUnitVec3i(), Blocks.SNOW_BLOCK, Blocks.POWDER_SNOW);
    // List<PlacementModifier> snowTreeFilterDecorator = List.of(
    //     EnvironmentScanPlacement.scanningFor(Direction.UP, BlockPredicate.not(BlockPredicate.matchesBlocks(Blocks.POWDER_SNOW)), 8),
    //     BlockPredicateFilter.forPredicate(snowTreePredicate)
    // );
    // =========================================================================

    auto snowTreePredicate = levelgen::blockpredicates::BlockPredicate::matchesBlocks(
        core::Vec3i(0, -1, 0),
        std::vector<std::string>{"minecraft:snow_block", "minecraft:powder_snow"}
    );
    s_environmentScanPlacements.push_back(EnvironmentScanPlacement::scanningFor(
        EnvironmentScanPlacement::Direction::UP,
        [](BlockState* state) {
            return state && state->getIdentifier() != "minecraft:powder_snow";
        },
        [](BlockState*) { return true; },
        8
    ));
    s_blockPredicateFilters.push_back(BlockPredicateFilter::forPredicate(snowTreePredicate));
    PINE_ON_SNOW = createPlaced(TreeFeatures::PINE, { &s_environmentScanPlacements.back(), &s_blockPredicateFilters.back() }, "PINE_ON_SNOW");
    SPRUCE_ON_SNOW = createPlaced(TreeFeatures::SPRUCE, { &s_environmentScanPlacements.back(), &s_blockPredicateFilters.back() }, "SPRUCE_ON_SNOW");

    // PINE_CHECKED
    PINE_CHECKED = createPlaced(TreeFeatures::PINE, filteredByBlockSurvival(spruceSapling), "PINE_CHECKED");

    // JUNGLE_TREE_CHECKED
    JUNGLE_TREE_CHECKED = createPlaced(TreeFeatures::JUNGLE_TREE, filteredByBlockSurvival(jungleSapling), "JUNGLE_TREE_CHECKED");

    // FANCY_OAK_CHECKED
    FANCY_OAK_CHECKED = createPlaced(TreeFeatures::FANCY_OAK, filteredByBlockSurvival(oakSapling), "FANCY_OAK_CHECKED");

    // MEGA_JUNGLE_TREE_CHECKED
    MEGA_JUNGLE_TREE_CHECKED = createPlaced(TreeFeatures::MEGA_JUNGLE_TREE, filteredByBlockSurvival(jungleSapling), "MEGA_JUNGLE_TREE_CHECKED");

    // MEGA_SPRUCE_CHECKED
    MEGA_SPRUCE_CHECKED = createPlaced(TreeFeatures::MEGA_SPRUCE, filteredByBlockSurvival(spruceSapling), "MEGA_SPRUCE_CHECKED");

    // MEGA_PINE_CHECKED
    MEGA_PINE_CHECKED = createPlaced(TreeFeatures::MEGA_PINE, filteredByBlockSurvival(spruceSapling), "MEGA_PINE_CHECKED");

    // TALL_MANGROVE_CHECKED
    TALL_MANGROVE_CHECKED = createPlaced(TreeFeatures::TALL_MANGROVE, filteredByBlockSurvival(mangrovePropagule), "TALL_MANGROVE_CHECKED");

    // JUNGLE_BUSH
    JUNGLE_BUSH = createPlaced(TreeFeatures::JUNGLE_BUSH, filteredByBlockSurvival(oakSapling), "JUNGLE_BUSH");

    // =========================================================================
    // BIRCH WITH BEES - Reference: Java lines 128-129
    // =========================================================================

    SUPER_BIRCH_BEES_0002 = createPlaced(TreeFeatures::SUPER_BIRCH_BEES_0002, filteredByBlockSurvival(birchSapling), "SUPER_BIRCH_BEES_0002");
    SUPER_BIRCH_BEES = createPlaced(TreeFeatures::SUPER_BIRCH_BEES, filteredByBlockSurvival(birchSapling), "SUPER_BIRCH_BEES");

    // =========================================================================
    // OAK WITH BEES - Reference: Java lines 130-131
    // =========================================================================

    OAK_BEES_0002_LEAF_LITTER = createPlaced(TreeFeatures::OAK_BEES_0002_LEAF_LITTER, filteredByBlockSurvival(oakSapling), "OAK_BEES_0002_LEAF_LITTER");
    OAK_BEES_002 = createPlaced(TreeFeatures::OAK_BEES_002, filteredByBlockSurvival(oakSapling), "OAK_BEES_002");

    // =========================================================================
    // BIRCH WITH BEES (regular) - Reference: Java lines 132-134
    // =========================================================================

    BIRCH_BEES_0002_PLACED = createPlaced(TreeFeatures::BIRCH_BEES_0002, filteredByBlockSurvival(birchSapling), "BIRCH_BEES_0002_PLACED");
    BIRCH_BEES_0002_LEAF_LITTER = createPlaced(TreeFeatures::BIRCH_BEES_0002_LEAF_LITTER, filteredByBlockSurvival(birchSapling), "BIRCH_BEES_0002_LEAF_LITTER");
    BIRCH_BEES_002 = createPlaced(TreeFeatures::BIRCH_BEES_002, filteredByBlockSurvival(birchSapling), "BIRCH_BEES_002");

    // =========================================================================
    // FANCY OAK WITH BEES - Reference: Java lines 135-137
    // =========================================================================

    FANCY_OAK_BEES_0002_LEAF_LITTER = createPlaced(TreeFeatures::FANCY_OAK_BEES_0002_LEAF_LITTER, filteredByBlockSurvival(oakSapling), "FANCY_OAK_BEES_0002_LEAF_LITTER");
    FANCY_OAK_BEES_002 = createPlaced(TreeFeatures::FANCY_OAK_BEES_002, filteredByBlockSurvival(oakSapling), "FANCY_OAK_BEES_002");
    FANCY_OAK_BEES = createPlaced(TreeFeatures::FANCY_OAK_BEES, filteredByBlockSurvival(oakSapling), "FANCY_OAK_BEES");

    // =========================================================================
    // CHERRY WITH BEES - Reference: Java line 138
    // =========================================================================

    CHERRY_BEES_005 = createPlaced(TreeFeatures::CHERRY_BEES_005, filteredByBlockSurvival(cherrySapling), "CHERRY_BEES_005");

    // =========================================================================
    // LEAF LITTER VARIANTS - Reference: Java lines 139-142
    // =========================================================================

    OAK_LEAF_LITTER = createPlaced(TreeFeatures::OAK_LEAF_LITTER, filteredByBlockSurvival(oakSapling), "OAK_LEAF_LITTER");
    DARK_OAK_LEAF_LITTER = createPlaced(TreeFeatures::DARK_OAK_LEAF_LITTER, filteredByBlockSurvival(darkOakSapling), "DARK_OAK_LEAF_LITTER");
    BIRCH_LEAF_LITTER = createPlaced(TreeFeatures::BIRCH_LEAF_LITTER, filteredByBlockSurvival(birchSapling), "BIRCH_LEAF_LITTER");
    FANCY_OAK_LEAF_LITTER = createPlaced(TreeFeatures::FANCY_OAK_LEAF_LITTER, filteredByBlockSurvival(oakSapling), "FANCY_OAK_LEAF_LITTER");

    // =========================================================================
    // FALLEN TREES - Reference: Java lines 143-147
    // =========================================================================

    FALLEN_OAK_TREE = createPlaced(TreeFeatures::FALLEN_OAK_TREE, filteredByBlockSurvival(oakSapling), "FALLEN_OAK_TREE");
    FALLEN_BIRCH_TREE = createPlaced(TreeFeatures::FALLEN_BIRCH_TREE, filteredByBlockSurvival(birchSapling), "FALLEN_BIRCH_TREE");
    FALLEN_SUPER_BIRCH_TREE = createPlaced(TreeFeatures::FALLEN_SUPER_BIRCH_TREE, filteredByBlockSurvival(birchSapling), "FALLEN_SUPER_BIRCH_TREE");
    FALLEN_SPRUCE_TREE = createPlaced(TreeFeatures::FALLEN_SPRUCE_TREE, filteredByBlockSurvival(spruceSapling), "FALLEN_SPRUCE_TREE");
    FALLEN_JUNGLE_TREE = createPlaced(TreeFeatures::FALLEN_JUNGLE_TREE, filteredByBlockSurvival(jungleSapling), "FALLEN_JUNGLE_TREE");

    s_initialized = true;
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
