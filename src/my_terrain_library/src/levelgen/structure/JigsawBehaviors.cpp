#include <mutex>
#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/structure/TemplatePool.h"
#include "levelgen/structure/ProcessorLists.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "data/worldgen/features/CaveFeatures.h"
#include "data/worldgen/features/VegetationFeatures.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <deque>
#include <map>
#include <memory>
#include <stdexcept>

// Reference: PoolElementStructurePiece.postProcess -> StructurePoolElement
// .place (B7 jigsaw block placement). SinglePoolElement: placeInWorld with
// knownShape + JigsawReplacement + the element's processor list + projection
// processors; LegacySinglePoolElement swaps the ignore processor to
// STRUCTURE_AND_AIR; FeaturePoolElement runs its placed feature on the piece
// stream; ListPoolElement sequences at the same position. handleDataMarker
// is the base-class no-op for all vanilla jigsaw structures.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;

// Village/outpost feature elements reference standalone placed features
// (EMPTY placement, or the sapling would-survive filter for trees) - NOT the
// biome variants. Built lazily; piles are village-only configured features.
placement::PlacedFeature* featureById(const std::string& featureId) {
    // Decoration now runs on several pool threads (ChunkStatusTasks
    // generateFeatures); this lazily-built registry raced and crashed.
    static std::mutex s_registryMutex;
    std::lock_guard<std::mutex> registryLock(s_registryMutex);
    using feature::stateproviders::SimpleStateProvider;
    using feature::stateproviders::WeightedStateProvider;
    using feature::stateproviders::WeightedStateEntry;
    using feature::stateproviders::RotatedBlockProvider;

    static std::map<std::string, placement::PlacedFeature*> registry;
    static std::deque<std::unique_ptr<placement::PlacedFeature>> ownedPlaced;
    static std::deque<std::unique_ptr<ConfiguredFeature>> ownedConfigured;
    static std::deque<std::unique_ptr<BlockPileConfiguration>> ownedPileConfigs;
    static std::deque<std::unique_ptr<placement::BlockPredicateFilter>> ownedFilters;
    static BlockPileFeature pileFeature;

    if (registry.empty()) {
        auto addPlaced = [&](const std::string& id, ConfiguredFeature* configured,
                             std::vector<placement::PlacementModifier*> modifiers) {
            ownedPlaced.push_back(std::make_unique<placement::PlacedFeature>(
                configured, modifiers, id));
            registry[id] = ownedPlaced.back().get();
        };
        auto pile = [&](const std::string& id,
                        std::shared_ptr<feature::stateproviders::BlockStateProvider> provider) {
            ownedPileConfigs.push_back(
                std::make_unique<BlockPileConfiguration>(std::move(provider)));
            ownedConfigured.push_back(
                std::make_unique<ConfiguredFeatureImpl<
                    BlockPileConfiguration, BlockPileFeature>>(
                    &pileFeature, *ownedPileConfigs.back()));
            addPlaced(id, ownedConfigured.back().get(), {});
        };
        auto saplingFiltered = [&](const std::string& id,
                                   ConfiguredFeature* configured,
                                   const std::string& saplingBlock) {
            // Reference: PlacementUtils.filteredByBlockSurvival(sapling).
            BlockState* sapling = Blocks::getDefaultState(saplingBlock);
            if (sapling == nullptr) {
                throw std::runtime_error("jigsaw feature sapling unregistered: "
                                         + saplingBlock);
            }
            auto wouldSurvive = blockpredicates::BlockPredicate::wouldSurvive(
                sapling, core::Vec3i::ZERO());
            ownedFilters.push_back(std::make_unique<placement::BlockPredicateFilter>(
                placement::BlockPredicateFilter::forPredicate(wouldSurvive)));
            addPlaced(id, configured, {ownedFilters.back().get()});
        };

        // Reference: data/minecraft/worldgen/placed_feature/<id>.json.
        addPlaced("minecraft:sculk_patch_ancient_city",
                  data::worldgen::features::CaveFeatures::SCULK_PATCH_ANCIENT_CITY, {});
        addPlaced("minecraft:flower_plain",
                  data::worldgen::features::VegetationFeatures::FLOWER_PLAIN, {});
        addPlaced("minecraft:patch_cactus",
                  data::worldgen::features::VegetationFeatures::PATCH_CACTUS, {});
        addPlaced("minecraft:patch_taiga_grass",
                  data::worldgen::features::VegetationFeatures::PATCH_TAIGA_GRASS, {});
        addPlaced("minecraft:patch_berry_bush",
                  data::worldgen::features::VegetationFeatures::PATCH_BERRY_BUSH, {});
        saplingFiltered("minecraft:oak", data::worldgen::features::TreeFeatures::OAK,
                        "minecraft:oak_sapling");
        saplingFiltered("minecraft:acacia", data::worldgen::features::TreeFeatures::ACACIA,
                        "minecraft:acacia_sapling");
        saplingFiltered("minecraft:pine", data::worldgen::features::TreeFeatures::PINE,
                        "minecraft:spruce_sapling");
        saplingFiltered("minecraft:spruce", data::worldgen::features::TreeFeatures::SPRUCE,
                        "minecraft:spruce_sapling");
        // Reference: PileFeatures - BLOCK_PILE with the exact providers.
        pile("minecraft:pile_hay", std::make_shared<RotatedBlockProvider>(
            Blocks::getDefaultState("minecraft:hay_block")));
        pile("minecraft:pile_melon", std::make_shared<SimpleStateProvider>(
            std::string("minecraft:melon")));
        pile("minecraft:pile_snow", std::make_shared<SimpleStateProvider>(
            std::string("minecraft:snow")));
        pile("minecraft:pile_ice", std::make_shared<WeightedStateProvider>(
            std::vector<WeightedStateEntry>{
                {Blocks::getDefaultState("minecraft:blue_ice"), 1},
                {Blocks::getDefaultState("minecraft:packed_ice"), 5}}));
        pile("minecraft:pile_pumpkin", std::make_shared<WeightedStateProvider>(
            std::vector<WeightedStateEntry>{
                {Blocks::getDefaultState("minecraft:pumpkin"), 19},
                {Blocks::getDefaultState("minecraft:jack_o_lantern"), 1}}));
    }

    auto it = registry.find(featureId);
    if (it == registry.end()) {
        throw std::runtime_error("jigsaw feature element not mapped: " + featureId);
    }
    return it->second;
}

class JigsawPieceBehavior final : public StructurePieceBehavior {
public:
    JigsawPieceBehavior(PoolElement element, core::BlockPos position, int rotation,
                        bool keepLiquids)
        : m_element(std::move(element)), m_position(position), m_rotation(rotation),
          m_keepLiquids(keepLiquids) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)chunkPos;
        (void)self;
        placeElement(m_element, level, generator, random, chunkBB, referencePos);
    }

private:
    PoolElement m_element;
    core::BlockPos m_position;
    int m_rotation;
    bool m_keepLiquids;

    // Reference: StructurePoolElement.place dispatch.
    void placeElement(const PoolElement& element, WorldGenLevel* level,
                      ChunkGenerator* generator, WorldgenRandom& random,
                      const BoundingBox& chunkBB, const core::BlockPos& referencePos) {
        switch (element.kind) {
            case PoolElementKind::EMPTY:
                return;
            case PoolElementKind::FEATURE:
                // Reference: FeaturePoolElement.place - the placed feature
                // runs on the piece stream at the piece position.
                featureById(element.feature)->place(level, generator, random, m_position);
                return;
            case PoolElementKind::LIST:
                // Reference: ListPoolElement.place - stop at the first failure.
                for (const PoolElement& sub : element.listElements) {
                    placeElement(sub, level, generator, random, chunkBB, referencePos);
                }
                return;
            case PoolElementKind::SINGLE:
            case PoolElementKind::LEGACY_SINGLE: {
                TemplatePlaceSettings settings;
                settings.rotation = m_rotation;
                settings.mirror = 0;
                settings.rotationPivot = core::BlockPos(0, 0, 0);
                // Reference: SinglePoolElement.getSettings - STRUCTURE_BLOCK
                // ignore (air places); legacy swaps to STRUCTURE_AND_AIR.
                settings.ignoreAir = element.kind == PoolElementKind::LEGACY_SINGLE;
                settings.keepLiquids = m_keepLiquids;
                settings.knownShape = true;
                settings.jigsawReplacement = true;
                if (!element.processors.empty()
                    && element.processors != "minecraft:empty") {
                    ProcessorLists::appendProcessors(element.processors, settings, level);
                }
                if (element.projection == "terrain_matching") {
                    settings.terrainMatchingGravity = true;
                    settings.gravityOffset = -1;
                }
                TemplateEngine::placeInWorld(level, element.location, m_position,
                                             referencePos, settings, random, chunkBB);
                return;
            }
        }
    }
};

} // namespace

std::shared_ptr<StructurePieceBehavior> jigsawPiece(const PoolElement& element,
                                                    const core::BlockPos& position,
                                                    int rotation, bool keepLiquids) {
    return std::make_shared<JigsawPieceBehavior>(element, position, rotation, keepLiquids);
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
