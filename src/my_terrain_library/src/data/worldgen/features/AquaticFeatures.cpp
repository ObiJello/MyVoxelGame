#include "data/worldgen/features/AquaticFeatures.h"
#include "levelgen/placement/PlacedFeature.h"

// MSVC's STL does not include <deque> transitively (libc++ does) — see the
// "MSVC compatibility fixes" list in CLAUDE.md.
#include <deque>

// Reference: net/minecraft/data/worldgen/features/AquaticFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace ::world;
using namespace levelgen;
using namespace levelgen::placement;
using Blocks = ::minecraft::world::level::block::Blocks;

// Static members
SeagrassFeature AquaticFeatures::s_seagrassFeature;
KelpFeature AquaticFeatures::s_kelpFeature;
bool AquaticFeatures::s_initialized = false;

// ConfiguredFeature pointers
ConfiguredFeature* AquaticFeatures::SEAGRASS_SHORT = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_SLIGHTLY_LESS_SHORT = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_MID = nullptr;
ConfiguredFeature* AquaticFeatures::SEAGRASS_TALL = nullptr;
ConfiguredFeature* AquaticFeatures::SEA_PICKLE = nullptr;
ConfiguredFeature* AquaticFeatures::KELP = nullptr;
ConfiguredFeature* AquaticFeatures::WARM_OCEAN_VEGETATION = nullptr;

// Storage for ConfiguredFeature instances
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<ProbabilityFeatureConfiguration>> s_seagrassConfigs;
static std::unique_ptr<NoneFeatureConfiguration> s_kelpConfig;

void AquaticFeatures::bootstrap() {
    if (s_initialized) return;

    // =========================================================================
    // SEAGRASS features
    // Reference: AquaticFeatures.java lines 24-27
    // =========================================================================

    // SEAGRASS_SHORT - probability 0.3
    // Reference: AquaticFeatures.java line 24
    {
        auto config = std::make_unique<ProbabilityFeatureConfiguration>(0.3f);
        auto feature = std::make_unique<ConfiguredFeatureImpl<ProbabilityFeatureConfiguration, SeagrassFeature>>(
            &s_seagrassFeature, *config);
        SEAGRASS_SHORT = feature.get();
        s_seagrassConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // SEAGRASS_SLIGHTLY_LESS_SHORT - probability 0.4
    // Reference: AquaticFeatures.java line 25
    {
        auto config = std::make_unique<ProbabilityFeatureConfiguration>(0.4f);
        auto feature = std::make_unique<ConfiguredFeatureImpl<ProbabilityFeatureConfiguration, SeagrassFeature>>(
            &s_seagrassFeature, *config);
        SEAGRASS_SLIGHTLY_LESS_SHORT = feature.get();
        s_seagrassConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // SEAGRASS_MID - probability 0.6
    // Reference: AquaticFeatures.java line 26
    {
        auto config = std::make_unique<ProbabilityFeatureConfiguration>(0.6f);
        auto feature = std::make_unique<ConfiguredFeatureImpl<ProbabilityFeatureConfiguration, SeagrassFeature>>(
            &s_seagrassFeature, *config);
        SEAGRASS_MID = feature.get();
        s_seagrassConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // SEAGRASS_TALL - probability 0.8
    // Reference: AquaticFeatures.java line 27
    {
        auto config = std::make_unique<ProbabilityFeatureConfiguration>(0.8f);
        auto feature = std::make_unique<ConfiguredFeatureImpl<ProbabilityFeatureConfiguration, SeagrassFeature>>(
            &s_seagrassFeature, *config);
        SEAGRASS_TALL = feature.get();
        s_seagrassConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // KELP feature
    // Reference: AquaticFeatures.java line 29
    // =========================================================================
    {
        s_kelpConfig = std::make_unique<NoneFeatureConfiguration>();
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, KelpFeature>>(
            &s_kelpFeature, *s_kelpConfig);
        KELP = feature.get();
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // SEA_PICKLE
    // Reference: AquaticFeatures.java line 28 / sea_pickle.json: count 20.
    // =========================================================================
    {
        static SeaPickleFeature s_seaPickleFeature;
        static std::vector<std::unique_ptr<CountConfiguration>> s_countConfigs;
        auto config = std::make_unique<CountConfiguration>(
            std::make_shared<util::ConstantInt>(20));
        auto feature = std::make_unique<ConfiguredFeatureImpl<CountConfiguration, SeaPickleFeature>>(
            &s_seaPickleFeature, *config);
        SEA_PICKLE = feature.get();
        s_countConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =========================================================================
    // WARM_OCEAN_VEGETATION
    // Reference: AquaticFeatures.java line 29 / warm_ocean_vegetation.json:
    // simple_random_selector over inline-placed coral_tree, coral_claw,
    // coral_mushroom (each with an empty placement list). The selector draws
    // nextInt(3) before the chosen coral feature runs.
    // =========================================================================
    {
        static CoralTreeFeature s_coralTreeFeature;
        static CoralClawFeature s_coralClawFeature;
        static CoralMushroomFeature s_coralMushroomFeature;
        static SimpleRandomSelectorFeature s_selectorFeature;
        static std::deque<PlacedFeature> s_coralPlaced;
        static std::vector<std::unique_ptr<NoneFeatureConfiguration>> s_coralNoneConfigs;
        static std::vector<std::unique_ptr<SimpleRandomFeatureConfiguration>> s_selectorConfigs;

        std::vector<PlacedFeature*> variants;
        auto addVariant = [&](auto* featurePtr, const std::string& name) {
            auto cfg = std::make_unique<NoneFeatureConfiguration>();
            auto configured = std::make_unique<ConfiguredFeatureImpl<
                NoneFeatureConfiguration, std::remove_pointer_t<decltype(featurePtr)>>>(
                featurePtr, *cfg);
            s_coralPlaced.emplace_back(configured.get(), std::vector<PlacementModifier*>{}, name);
            variants.push_back(&s_coralPlaced.back());
            s_coralNoneConfigs.push_back(std::move(cfg));
            s_features.push_back(std::move(configured));
        };
        addVariant(&s_coralTreeFeature, "CORAL_TREE_INLINE");
        addVariant(&s_coralClawFeature, "CORAL_CLAW_INLINE");
        addVariant(&s_coralMushroomFeature, "CORAL_MUSHROOM_INLINE");

        auto selectorConfig = std::make_unique<SimpleRandomFeatureConfiguration>(variants);
        auto selector = std::make_unique<ConfiguredFeatureImpl<
            SimpleRandomFeatureConfiguration,
            SimpleRandomSelectorFeature>>(&s_selectorFeature, *selectorConfig);
        WARM_OCEAN_VEGETATION = selector.get();
        s_selectorConfigs.push_back(std::move(selectorConfig));
        s_features.push_back(std::move(selector));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
