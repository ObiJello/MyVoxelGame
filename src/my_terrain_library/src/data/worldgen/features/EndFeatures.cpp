#include "data/worldgen/features/EndFeatures.h"

// Reference: net/minecraft/data/worldgen/features/EndFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;

EndPlatformFeature EndFeatures::s_endPlatformFeature;
EndSpikeFeature EndFeatures::s_endSpikeFeature;
EndGatewayFeature EndFeatures::s_endGatewayFeature;
ChorusPlantFeature EndFeatures::s_chorusPlantFeature;
EndIslandFeature EndFeatures::s_endIslandFeature;
bool EndFeatures::s_initialized = false;

ConfiguredFeature* EndFeatures::END_PLATFORM = nullptr;
ConfiguredFeature* EndFeatures::END_SPIKE = nullptr;
ConfiguredFeature* EndFeatures::END_GATEWAY_RETURN = nullptr;
ConfiguredFeature* EndFeatures::CHORUS_PLANT = nullptr;
ConfiguredFeature* EndFeatures::END_ISLAND = nullptr;

static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;

void EndFeatures::bootstrap() {
    if (s_initialized) return;

    // END_PLATFORM - Reference: EndFeatures.java line 23
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, EndPlatformFeature>>(
            &s_endPlatformFeature, NoneFeatureConfiguration());
        END_PLATFORM = feature.get();
        s_features.push_back(std::move(feature));
    }

    // END_SPIKE - line 24: SpikeConfiguration(false, [], null)
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<EndSpikeConfiguration, EndSpikeFeature>>(
            &s_endSpikeFeature, EndSpikeConfiguration(false));
        END_SPIKE = feature.get();
        s_features.push_back(std::move(feature));
    }

    // END_GATEWAY_RETURN - line 25: knownExit(END_SPAWN_POINT(100,50,0), true)
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<EndGatewayConfiguration, EndGatewayFeature>>(
            &s_endGatewayFeature, EndGatewayConfiguration(core::BlockPos(100, 50, 0), true));
        END_GATEWAY_RETURN = feature.get();
        s_features.push_back(std::move(feature));
    }

    // CHORUS_PLANT - line 27
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, ChorusPlantFeature>>(
            &s_chorusPlantFeature, NoneFeatureConfiguration());
        CHORUS_PLANT = feature.get();
        s_features.push_back(std::move(feature));
    }

    // END_ISLAND - line 28
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, EndIslandFeature>>(
            &s_endIslandFeature, NoneFeatureConfiguration());
        END_ISLAND = feature.get();
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
