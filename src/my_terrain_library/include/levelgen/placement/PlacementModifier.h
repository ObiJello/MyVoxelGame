#pragma once

#include "core/BlockPos.h"
#include "levelgen/WorldgenRandom.h"
#include <vector>
#include <cstdint>
#include <string>

// Reference: net/minecraft/world/level/levelgen/placement/PlacementModifier.java
// Reference: net/minecraft/world/level/levelgen/placement/PlacementFilter.java
// Reference: net/minecraft/world/level/levelgen/placement/RepeatingPlacement.java

namespace minecraft {
namespace levelgen {
namespace placement {

// Forward declaration
class PlacementContext;

/**
 * PlacementModifier - Base class for feature placement modifiers
 * Transforms a stream of positions into another stream of positions
 * Reference: PlacementModifier.java
 */
class PlacementModifier {
public:
    virtual ~PlacementModifier() = default;

    /**
     * Get positions for feature placement
     * Reference: PlacementModifier.java line 12
     *
     * @param context The placement context
     * @param random Random source for stochastic placement
     * @param origin The input position
     * @return Vector of positions (may be empty, single, or multiple)
     */
    virtual std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) = 0;

    /**
     * Get type name for debugging/logging
     * Override in derived classes to return the modifier type name
     */
    virtual std::string getTypeName() const { return "PlacementModifier"; }
};

/**
 * PlacementFilter - Base class for filtering placements
 * Either passes through the origin or returns empty
 * Reference: PlacementFilter.java
 */
class PlacementFilter : public PlacementModifier {
public:
    /**
     * Get positions - passes through if shouldPlace returns true
     * Reference: PlacementFilter.java lines 8-10
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        if (shouldPlace(context, random, origin)) {
            return {origin};
        }
        return {};
    }

    std::string getTypeName() const override { return "PlacementFilter"; }

protected:
    /**
     * Check if placement should occur at this position
     * Reference: PlacementFilter.java line 12
     */
    virtual bool shouldPlace(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) = 0;
};

/**
 * RepeatingPlacement - Base class for count-based placement
 * Returns the origin position N times
 * Reference: RepeatingPlacement.java
 */
class RepeatingPlacement : public PlacementModifier {
public:
    /**
     * Get positions - returns origin count() times
     * Reference: RepeatingPlacement.java lines 11-13
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) override {
        int32_t n = count(random, origin);
        // Java's IntStream.range(0, n) returns empty stream for n <= 0
        if (n <= 0) return {};
        return std::vector<core::BlockPos>(n, origin);
    }

    std::string getTypeName() const override { return "RepeatingPlacement"; }

protected:
    /**
     * Get the number of times to repeat
     * Reference: RepeatingPlacement.java line 9
     */
    virtual int32_t count(WorldgenRandom& random, const core::BlockPos& origin) = 0;
};

} // namespace placement
} // namespace levelgen
} // namespace minecraft
