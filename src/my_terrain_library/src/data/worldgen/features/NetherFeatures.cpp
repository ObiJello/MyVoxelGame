#include "data/worldgen/features/NetherFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "util/IntProvider.h"

// Reference: net/minecraft/data/worldgen/features/NetherFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace levelgen::placement;
using minecraft::util::ConstantInt;
using minecraft::util::UniformInt;

// Feature instances
DeltaFeature NetherFeatures::s_deltaFeature;
BasaltColumnsFeature NetherFeatures::s_basaltColumnsFeature;
ReplaceBlobsFeature NetherFeatures::s_replaceBlobsFeature;
GlowstoneFeature NetherFeatures::s_glowstoneFeature;
NetherForestVegetationFeature NetherFeatures::s_netherForestVegetationFeature;
TwistingVinesFeature NetherFeatures::s_twistingVinesFeature;
WeepingVinesFeature NetherFeatures::s_weepingVinesFeature;
BasaltPillarFeature NetherFeatures::s_basaltPillarFeature;
SpringFeature NetherFeatures::s_springFeature;
RandomPatchFeature NetherFeatures::s_randomPatchFeature;
SimpleBlockFeature NetherFeatures::s_simpleBlockFeature;
bool NetherFeatures::s_initialized = false;

// Configured feature pointers
ConfiguredFeature* NetherFeatures::DELTA = nullptr;
ConfiguredFeature* NetherFeatures::SMALL_BASALT_COLUMNS = nullptr;
ConfiguredFeature* NetherFeatures::LARGE_BASALT_COLUMNS = nullptr;
ConfiguredFeature* NetherFeatures::BASALT_BLOBS = nullptr;
ConfiguredFeature* NetherFeatures::BLACKSTONE_BLOBS = nullptr;
ConfiguredFeature* NetherFeatures::GLOWSTONE_EXTRA = nullptr;
ConfiguredFeature* NetherFeatures::CRIMSON_FOREST_VEGETATION = nullptr;
ConfiguredFeature* NetherFeatures::WARPED_FOREST_VEGETATION = nullptr;
ConfiguredFeature* NetherFeatures::NETHER_SPROUTS = nullptr;
ConfiguredFeature* NetherFeatures::TWISTING_VINES = nullptr;
ConfiguredFeature* NetherFeatures::WEEPING_VINES = nullptr;
ConfiguredFeature* NetherFeatures::PATCH_CRIMSON_ROOTS = nullptr;
ConfiguredFeature* NetherFeatures::BASALT_PILLAR = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_LAVA_NETHER = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_NETHER_CLOSED = nullptr;
ConfiguredFeature* NetherFeatures::SPRING_NETHER_OPEN = nullptr;
ConfiguredFeature* NetherFeatures::PATCH_FIRE = nullptr;
ConfiguredFeature* NetherFeatures::PATCH_SOUL_FIRE = nullptr;

// Owned storage
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::vector<std::unique_ptr<PlacementModifier>> s_placementModifiers;
static std::vector<std::shared_ptr<feature::stateproviders::BlockStateProvider>> s_stateProviders;
static std::vector<std::unique_ptr<SpringConfiguration>> s_springConfigs;
static std::vector<std::unique_ptr<RandomPatchConfiguration>> s_patchConfigs;

void NetherFeatures::bootstrap() {
    if (s_initialized) return;

    auto block = [](const char* name) -> BlockState* {
        return minecraft::world::level::block::Blocks::getDefaultState(name);
    };

    // ---- helpers mirroring FeatureUtils / VegetationFeatures.cpp patterns ----
    auto storeModifier = [](std::unique_ptr<PlacementModifier> mod) -> PlacementModifier* {
        PlacementModifier* raw = mod.get();
        s_placementModifiers.push_back(std::move(mod));
        return raw;
    };

    auto createPlacedFeature = [](ConfiguredFeature* feature,
                                  const std::vector<PlacementModifier*>& modifiers,
                                  const std::string& name = "") -> PlacedFeature* {
        auto placed = std::make_unique<PlacedFeature>(feature, modifiers, name);
        PlacedFeature* raw = placed.get();
        s_placedFeatures.push_back(std::move(placed));
        return raw;
    };

    auto createSimpleBlockConfiguredFeature =
        [&](std::shared_ptr<feature::stateproviders::BlockStateProvider> provider) -> ConfiguredFeature* {
        s_stateProviders.push_back(std::move(provider));
        auto* rawProvider = s_stateProviders.back().get();
        auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature,
            SimpleBlockConfiguration(rawProvider, false)
        );
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };

    // FeatureUtils.simplePatchConfiguration: tries 96, xz 7, y 3; inner placed
    // feature filtered by ONLY_IN_AIR (+ matchesBlocks(0,-1,0, allowedOn)).
    auto simplePatchConfiguration =
        [&](std::shared_ptr<feature::stateproviders::BlockStateProvider> provider,
            const std::vector<std::string>& allowedOnNames) -> RandomPatchConfiguration* {
        std::shared_ptr<blockpredicates::BlockPredicate> predicate;
        if (allowedOnNames.empty()) {
            predicate = blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE;
        } else {
            predicate = blockpredicates::BlockPredicate::allOf(
                blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE,
                blockpredicates::BlockPredicate::matchesBlocks(core::Vec3i(0, -1, 0), allowedOnNames)
            );
        }
        PlacedFeature* inner = createPlacedFeature(
            createSimpleBlockConfiguredFeature(std::move(provider)),
            {storeModifier(std::make_unique<BlockPredicateFilter>(
                BlockPredicateFilter::forPredicate(predicate)))}
        );
        auto config = std::make_unique<RandomPatchConfiguration>(96, 7, 3, inner);
        RandomPatchConfiguration* raw = config.get();
        s_patchConfigs.push_back(std::move(config));
        return raw;
    };

    // ---- configured features, in Java bootstrap order ----

    // DELTA - Reference: NetherFeatures.java line 50
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<DeltaFeatureConfiguration, DeltaFeature>>(
            &s_deltaFeature,
            DeltaFeatureConfiguration(
                block("minecraft:lava"),
                block("minecraft:magma_block"),
                std::make_shared<UniformInt>(3, 7),
                std::make_shared<UniformInt>(0, 2)
            )
        );
        DELTA = feature.get();
        s_features.push_back(std::move(feature));
    }

    // SMALL_BASALT_COLUMNS / LARGE_BASALT_COLUMNS - lines 51-52
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<ColumnFeatureConfiguration, BasaltColumnsFeature>>(
            &s_basaltColumnsFeature,
            ColumnFeatureConfiguration(
                std::make_shared<ConstantInt>(1),
                std::make_shared<UniformInt>(1, 4)
            )
        );
        SMALL_BASALT_COLUMNS = feature.get();
        s_features.push_back(std::move(feature));
    }
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<ColumnFeatureConfiguration, BasaltColumnsFeature>>(
            &s_basaltColumnsFeature,
            ColumnFeatureConfiguration(
                std::make_shared<UniformInt>(2, 3),
                std::make_shared<UniformInt>(5, 10)
            )
        );
        LARGE_BASALT_COLUMNS = feature.get();
        s_features.push_back(std::move(feature));
    }

    // BASALT_BLOBS / BLACKSTONE_BLOBS - lines 53-54
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<ReplaceSphereConfiguration, ReplaceBlobsFeature>>(
            &s_replaceBlobsFeature,
            ReplaceSphereConfiguration(
                block("minecraft:netherrack"),
                block("minecraft:basalt"),
                std::make_shared<UniformInt>(3, 7)
            )
        );
        BASALT_BLOBS = feature.get();
        s_features.push_back(std::move(feature));
    }
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<ReplaceSphereConfiguration, ReplaceBlobsFeature>>(
            &s_replaceBlobsFeature,
            ReplaceSphereConfiguration(
                block("minecraft:netherrack"),
                block("minecraft:blackstone"),
                std::make_shared<UniformInt>(3, 7)
            )
        );
        BLACKSTONE_BLOBS = feature.get();
        s_features.push_back(std::move(feature));
    }

    // GLOWSTONE_EXTRA - line 55
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, GlowstoneFeature>>(
            &s_glowstoneFeature,
            NoneFeatureConfiguration()
        );
        GLOWSTONE_EXTRA = feature.get();
        s_features.push_back(std::move(feature));
    }

    // Forest vegetation - lines 56-63
    {
        auto crimsonProvider = std::make_shared<feature::stateproviders::WeightedStateProvider>(
            std::vector<feature::stateproviders::WeightedStateEntry>{
                {block("minecraft:crimson_roots"), 87},
                {block("minecraft:crimson_fungus"), 11},
                {block("minecraft:warped_fungus"), 1}
            });
        s_stateProviders.push_back(crimsonProvider);
        auto feature = std::make_unique<ConfiguredFeatureImpl<NetherForestVegetationConfiguration, NetherForestVegetationFeature>>(
            &s_netherForestVegetationFeature,
            NetherForestVegetationConfiguration(crimsonProvider, 8, 4)
        );
        CRIMSON_FOREST_VEGETATION = feature.get();
        s_features.push_back(std::move(feature));
    }
    {
        auto warpedProvider = std::make_shared<feature::stateproviders::WeightedStateProvider>(
            std::vector<feature::stateproviders::WeightedStateEntry>{
                {block("minecraft:warped_roots"), 85},
                {block("minecraft:crimson_roots"), 1},
                {block("minecraft:warped_fungus"), 13},
                {block("minecraft:crimson_fungus"), 1}
            });
        s_stateProviders.push_back(warpedProvider);
        auto feature = std::make_unique<ConfiguredFeatureImpl<NetherForestVegetationConfiguration, NetherForestVegetationFeature>>(
            &s_netherForestVegetationFeature,
            NetherForestVegetationConfiguration(warpedProvider, 8, 4)
        );
        WARPED_FOREST_VEGETATION = feature.get();
        s_features.push_back(std::move(feature));
    }
    {
        auto sproutsProvider = std::make_shared<feature::stateproviders::SimpleStateProvider>(
            block("minecraft:nether_sprouts"));
        s_stateProviders.push_back(sproutsProvider);
        auto feature = std::make_unique<ConfiguredFeatureImpl<NetherForestVegetationConfiguration, NetherForestVegetationFeature>>(
            &s_netherForestVegetationFeature,
            NetherForestVegetationConfiguration(sproutsProvider, 8, 4)
        );
        NETHER_SPROUTS = feature.get();
        s_features.push_back(std::move(feature));
    }

    // TWISTING_VINES - line 64: TwistingVinesConfig(8, 4, 8)
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<TwistingVinesConfiguration, TwistingVinesFeature>>(
            &s_twistingVinesFeature,
            TwistingVinesConfiguration(8, 4, 8)
        );
        TWISTING_VINES = feature.get();
        s_features.push_back(std::move(feature));
    }

    // WEEPING_VINES - line 66
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, WeepingVinesFeature>>(
            &s_weepingVinesFeature,
            NoneFeatureConfiguration()
        );
        WEEPING_VINES = feature.get();
        s_features.push_back(std::move(feature));
    }

    // PATCH_CRIMSON_ROOTS - line 67 (no allowedOn list)
    {
        auto rootsProvider = std::make_shared<feature::stateproviders::SimpleStateProvider>(
            block("minecraft:crimson_roots"));
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature,
            *simplePatchConfiguration(rootsProvider, {})
        );
        PATCH_CRIMSON_ROOTS = feature.get();
        s_features.push_back(std::move(feature));
    }

    // BASALT_PILLAR - line 68
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, BasaltPillarFeature>>(
            &s_basaltPillarFeature,
            NoneFeatureConfiguration()
        );
        BASALT_PILLAR = feature.get();
        s_features.push_back(std::move(feature));
    }

    // Springs - lines 69-71
    auto makeSpring = [&](bool requiresBlockBelow, int rockCount, int holeCount,
                          const std::set<std::string>& validBlocks) -> ConfiguredFeature* {
        auto config = std::make_unique<SpringConfiguration>(
            "minecraft:lava", requiresBlockBelow, rockCount, holeCount, validBlocks);
        auto feature = std::make_unique<ConfiguredFeatureImpl<SpringConfiguration, SpringFeature>>(
            &s_springFeature, *config);
        ConfiguredFeature* raw = feature.get();
        s_springConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
        return raw;
    };
    SPRING_LAVA_NETHER = makeSpring(true, 4, 1,
        {"minecraft:netherrack", "minecraft:soul_sand", "minecraft:gravel",
         "minecraft:magma_block", "minecraft:blackstone"});
    SPRING_NETHER_CLOSED = makeSpring(false, 5, 0, {"minecraft:netherrack"});
    SPRING_NETHER_OPEN = makeSpring(false, 4, 1, {"minecraft:netherrack"});

    // PATCH_FIRE / PATCH_SOUL_FIRE - lines 72-73
    {
        auto fireProvider = std::make_shared<feature::stateproviders::SimpleStateProvider>(
            block("minecraft:fire"));
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature,
            *simplePatchConfiguration(fireProvider, {"minecraft:netherrack"})
        );
        PATCH_FIRE = feature.get();
        s_features.push_back(std::move(feature));
    }
    {
        auto soulFireProvider = std::make_shared<feature::stateproviders::SimpleStateProvider>(
            block("minecraft:soul_fire"));
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature,
            *simplePatchConfiguration(soulFireProvider, {"minecraft:soul_soil"})
        );
        PATCH_SOUL_FIRE = feature.get();
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
