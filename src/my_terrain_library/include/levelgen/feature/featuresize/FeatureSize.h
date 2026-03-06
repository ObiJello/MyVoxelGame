#pragma once

#include <cstdint>
#include <optional>

// Reference: net/minecraft/world/level/levelgen/feature/featuresize/FeatureSize.java
// Reference: net/minecraft/world/level/levelgen/feature/featuresize/TwoLayersFeatureSize.java
// Reference: net/minecraft/world/level/levelgen/feature/featuresize/ThreeLayersFeatureSize.java

namespace minecraft {
namespace levelgen {
namespace feature {
namespace featuresize {

/**
 * FeatureSize - Determines tree width at different heights
 * Reference: FeatureSize.java
 *
 * Used to check if a tree has enough space to generate.
 */
class FeatureSize {
protected:
    static constexpr int MAX_WIDTH = 16;
    std::optional<int> m_minClippedHeight;

public:
    explicit FeatureSize(std::optional<int> minClippedHeight = std::nullopt)
        : m_minClippedHeight(minClippedHeight) {}

    virtual ~FeatureSize() = default;

    /**
     * Get the required width at a given height
     * Reference: FeatureSize.java line 24
     *
     * @param treeHeight Total height of the tree
     * @param yo Y offset from base
     * @return Required width at this height
     */
    virtual int getSizeAtHeight(int treeHeight, int yo) const = 0;

    /**
     * Get minimum clipped height
     * Reference: FeatureSize.java line 26-28
     */
    std::optional<int> minClippedHeight() const {
        return m_minClippedHeight;
    }
};

/**
 * TwoLayersFeatureSize - Two-zone width calculation
 * Reference: TwoLayersFeatureSize.java
 *
 * Below limit: returns lowerSize
 * At/above limit: returns upperSize
 */
class TwoLayersFeatureSize : public FeatureSize {
private:
    int m_limit;
    int m_lowerSize;
    int m_upperSize;

public:
    /**
     * Constructor with default empty minClippedHeight
     * Reference: TwoLayersFeatureSize.java lines 14-16
     */
    TwoLayersFeatureSize(int limit, int lowerSize, int upperSize)
        : FeatureSize(std::nullopt)
        , m_limit(limit)
        , m_lowerSize(lowerSize)
        , m_upperSize(upperSize)
    {}

    /**
     * Constructor with minClippedHeight
     * Reference: TwoLayersFeatureSize.java lines 18-23
     */
    TwoLayersFeatureSize(int limit, int lowerSize, int upperSize, std::optional<int> minClippedHeight)
        : FeatureSize(minClippedHeight)
        , m_limit(limit)
        , m_lowerSize(lowerSize)
        , m_upperSize(upperSize)
    {}

    /**
     * Get size at height
     * Reference: TwoLayersFeatureSize.java lines 29-31
     */
    int getSizeAtHeight(int treeHeight, int yo) const override {
        return yo < m_limit ? m_lowerSize : m_upperSize;
    }
};

/**
 * ThreeLayersFeatureSize - Three-zone width calculation
 * Reference: ThreeLayersFeatureSize.java
 *
 * Below lowerLimit: returns lowerSize
 * Between lowerLimit and upperLimit: returns middleSize
 * At/above upperLimit: returns upperSize
 */
class ThreeLayersFeatureSize : public FeatureSize {
private:
    int m_limit;
    int m_upperLimit;
    int m_lowerSize;
    int m_middleSize;
    int m_upperSize;

public:
    /**
     * Constructor
     * Reference: ThreeLayersFeatureSize.java
     */
    ThreeLayersFeatureSize(
        int limit,
        int upperLimit,
        int lowerSize,
        int middleSize,
        int upperSize,
        std::optional<int> minClippedHeight = std::nullopt
    )
        : FeatureSize(minClippedHeight)
        , m_limit(limit)
        , m_upperLimit(upperLimit)
        , m_lowerSize(lowerSize)
        , m_middleSize(middleSize)
        , m_upperSize(upperSize)
    {}

    /**
     * Get size at height
     * Reference: ThreeLayersFeatureSize.java getSizeAtHeight()
     */
    int getSizeAtHeight(int treeHeight, int yo) const override {
        if (yo < m_limit) {
            return m_lowerSize;
        } else if (yo >= treeHeight - m_upperLimit) {
            return m_upperSize;
        } else {
            return m_middleSize;
        }
    }
};

} // namespace featuresize
} // namespace feature
} // namespace levelgen
} // namespace minecraft
