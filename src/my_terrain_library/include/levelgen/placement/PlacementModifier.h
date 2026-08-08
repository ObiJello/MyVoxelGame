#pragma once

#include "core/BlockPos.h"
#include "levelgen/placement/PlacementContext.h"
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
     * Appends results to `out` (may append zero, one, or many positions).
     * Emission order is the parity-relevant Java stream order.
     *
     * @param context The placement context
     * @param random Random source for stochastic placement
     * @param origin The input position
     * @param out Output buffer positions are appended to
     */
    virtual void appendPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        std::vector<core::BlockPos>& out
    ) = 0;

    /**
     * Allocating convenience wrapper around appendPositions().
     * Debug/trace paths only — the generation hot path uses appendPositions
     * with a reused buffer.
     */
    std::vector<core::BlockPos> getPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin
    ) {
        std::vector<core::BlockPos> out;
        appendPositions(context, random, origin, out);
        return out;
    }

    /**
     * Optional trace details for logging/debugging.
     * This runs after getPositions() and must not mutate state.
     */
    virtual std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const {
        (void)context;
        (void)origin;
        (void)results;
        return "";
    }

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
    void appendPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        std::vector<core::BlockPos>& out
    ) override {
        if (shouldPlace(context, random, origin)) {
            out.push_back(origin);
        }
    }

    std::string getTypeName() const override { return "PlacementFilter"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return std::string("accepted=") + (results.empty() ? "false" : "true");
    }

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
    void appendPositions(
        PlacementContext& context,
        WorldgenRandom& random,
        const core::BlockPos& origin,
        std::vector<core::BlockPos>& out
    ) override {
        int32_t n = count(random, origin);
        // Java's IntStream.range(0, n) returns empty stream for n <= 0
        for (int32_t i = 0; i < n; ++i) {
            out.push_back(origin);
        }
    }

    std::string getTypeName() const override { return "RepeatingPlacement"; }

    std::string describeTrace(
        PlacementContext& context,
        const core::BlockPos& origin,
        const std::vector<core::BlockPos>& results
    ) const override {
        (void)context;
        (void)origin;
        return "count=" + std::to_string(results.size());
    }

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
