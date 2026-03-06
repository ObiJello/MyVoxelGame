#include "data/worldgen/features/AquaticFeatures.h"
#include "levelgen/placement/PlacedFeature.h"

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

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
