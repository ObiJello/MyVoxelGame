#pragma once

#include "levelgen/placement/PlacementModifier.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "core/BlockPos.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <ostream>

// Reference: net/minecraft/world/level/levelgen/placement/PlacedFeature.java

namespace minecraft {
namespace levelgen {

// Forward declarations
class ConfiguredFeature;
class ChunkGenerator;

namespace placement {

/**
 * PlacedFeature - A configured feature with placement modifiers
 * Reference: PlacedFeature.java
 */
class PlacedFeature {
private:
    ConfiguredFeature* m_feature;
    std::vector<PlacementModifier*> m_placement;
    std::string m_name;  // Feature name for logging/debugging

public:
    /**
     * Constructor
     * Reference: PlacedFeature.java record constructor
     */
    PlacedFeature(ConfiguredFeature* feature, const std::vector<PlacementModifier*>& placement)
        : m_feature(feature)
        , m_placement(placement)
        , m_name("")
    {}

    /**
     * Constructor with name
     */
    PlacedFeature(ConfiguredFeature* feature, const std::vector<PlacementModifier*>& placement, const std::string& name)
        : m_feature(feature)
        , m_placement(placement)
        , m_name(name)
    {}

    /**
     * Place the feature without biome check
     * Reference: PlacedFeature.java lines 28-30
     */
    bool place(
        WorldGenLevel* level,
        ChunkGenerator* generator,
        WorldgenRandom& random,
        const core::BlockPos& origin
    );

    /**
     * Place the feature with biome check
     * Reference: PlacedFeature.java lines 32-34
     */
    bool placeWithBiomeCheck(
        WorldGenLevel* level,
        ChunkGenerator* generator,
        WorldgenRandom& random,
        const core::BlockPos& origin
    );

    /**
     * Get the configured feature
     */
    ConfiguredFeature* feature() const { return m_feature; }

    /**
     * Get the placement modifiers
     */
    const std::vector<PlacementModifier*>& placement() const { return m_placement; }

    /**
     * Get feature name
     */
    const std::string& getName() const { return m_name; }

    /**
     * Set feature name
     */
    void setName(const std::string& name) { m_name = name; }

    //=========================================================================
    // Static Feature Logging Control
    //=========================================================================

    /**
     * Enable/disable feature logging
     */
    static void setLoggingEnabled(bool enabled);

    /**
     * Set detail level for logging
     * 0 = basic (just step/index and result)
     * 1 = positions (show generated positions)
     * 2 = verbose (show more positions and modifier info)
     */
    static void setDetailLevel(int level);

    /**
     * Get current detail level
     */
    static int getDetailLevel();

    /**
     * Check if logging is enabled
     */
    static bool isLoggingEnabled();

    /**
     * Set logging output stream (default is std::cerr)
     */
    static void setLogStream(std::ostream* stream);

    /**
     * Get current log stream
     */
    static std::ostream* getLogStream();

    /**
     * Set current step/index for logging (call before each feature placement)
     */
    static void setCurrentStepIndex(int step, int index);

    /**
     * Get current step
     */
    static int getCurrentStep();

    /**
     * Get current index
     */
    static int getCurrentIndex();

    /**
     * Set modifier-level tracing (detailed per-modifier output)
     * @param enabled Enable/disable modifier tracing
     */
    static void setModifierTracingEnabled(bool enabled);

    /**
     * Check if modifier tracing is enabled
     */
    static bool isModifierTracingEnabled();

private:
    /**
     * Internal placement implementation
     * Reference: PlacedFeature.java lines 36-55
     */
    bool placeWithContext(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    );
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
