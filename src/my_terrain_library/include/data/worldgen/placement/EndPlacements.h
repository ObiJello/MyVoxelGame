#pragma once

#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "data/worldgen/features/EndFeatures.h"
#include <vector>
#include <memory>

// Reference: net/minecraft/data/worldgen/placement/EndPlacements.java

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

using namespace levelgen;
using namespace levelgen::placement;

/**
 * EndPlacements - Registry of placed End features
 * Reference: EndPlacements.java
 */
class EndPlacements {
private:
    static bool s_initialized;

public:
    static PlacedFeature* END_PLATFORM;
    static PlacedFeature* END_SPIKE;
    static PlacedFeature* END_GATEWAY_RETURN;
    static PlacedFeature* CHORUS_PLANT;
    static PlacedFeature* END_ISLAND_DECORATED;

    static void bootstrap();
    static bool isInitialized() { return s_initialized; }
};

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
