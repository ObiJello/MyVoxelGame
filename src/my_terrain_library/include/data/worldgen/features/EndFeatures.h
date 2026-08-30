#pragma once

#include "levelgen/feature/Feature.h"
#include "world/level/block/Blocks.h"
#include <memory>
#include <vector>

// Reference: net/minecraft/data/worldgen/features/EndFeatures.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

/**
 * EndFeatures - Registry of End configured features
 * Reference: EndFeatures.java
 */
class EndFeatures {
private:
    static levelgen::EndPlatformFeature s_endPlatformFeature;
    static levelgen::EndSpikeFeature s_endSpikeFeature;
    static levelgen::EndGatewayFeature s_endGatewayFeature;
    static levelgen::ChorusPlantFeature s_chorusPlantFeature;
    static levelgen::EndIslandFeature s_endIslandFeature;

    static bool s_initialized;

public:
    static levelgen::ConfiguredFeature* END_PLATFORM;
    static levelgen::ConfiguredFeature* END_SPIKE;
    static levelgen::ConfiguredFeature* END_GATEWAY_RETURN;
    static levelgen::ConfiguredFeature* CHORUS_PLANT;
    static levelgen::ConfiguredFeature* END_ISLAND;

    static void bootstrap();
    static bool isInitialized() { return s_initialized; }
};

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
