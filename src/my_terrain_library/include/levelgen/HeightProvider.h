#pragma once

#include "levelgen/VerticalAnchor.h"
#include "levelgen/SurfaceRules.h"
#include "math/Mth.h"
#include <cstdint>
#include <memory>

// Reference: net/minecraft/world/level/levelgen/heightproviders/HeightProvider.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/ConstantHeight.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/UniformHeight.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/TrapezoidHeight.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/BiasedToBottomHeight.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/VeryBiasedToBottomHeight.java

namespace minecraft {
namespace levelgen {

/**
 * HeightProvider - Abstract base class for height value providers
 * Reference: HeightProvider.java
 */
class HeightProvider {
public:
    virtual ~HeightProvider() = default;

    /**
     * Sample a Y coordinate from this provider
     * Reference: HeightProvider.java line 14
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        return sampleImpl(&random, context);
    }

protected:
    virtual int32_t sampleImpl(void* random, const WorldGenerationContext& context) = 0;
};

/**
 * ConstantHeight - Returns a constant height value
 * Reference: ConstantHeight.java
 */
class ConstantHeight : public HeightProvider {
private:
    VerticalAnchor m_value;

public:
    static ConstantHeight ZERO;

    /**
     * Factory method
     * Reference: ConstantHeight.java lines 13-15
     */
    static ConstantHeight of(const VerticalAnchor& value) {
        return ConstantHeight(value);
    }

    explicit ConstantHeight(const VerticalAnchor& value) : m_value(value) {}

    const VerticalAnchor& getValue() const { return m_value; }

    /**
     * Sample - returns resolved anchor value
     * Reference: ConstantHeight.java lines 25-27
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& /*random*/, const WorldGenerationContext& context) {
        return m_value.resolveY(context);
    }

protected:
    int32_t sampleImpl(void* /*random*/, const WorldGenerationContext& context) override {
        return m_value.resolveY(context);
    }
};

/**
 * UniformHeight - Returns uniform random height in range
 * Reference: UniformHeight.java
 */
class UniformHeight : public HeightProvider {
private:
    VerticalAnchor m_minInclusive;
    VerticalAnchor m_maxInclusive;

public:
    /**
     * Factory method
     * Reference: UniformHeight.java lines 26-28
     */
    static UniformHeight of(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        return UniformHeight(minInclusive, maxInclusive);
    }

    UniformHeight(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
    {}

    /**
     * Sample - returns uniform random height in range
     * Reference: UniformHeight.java lines 30-42
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (min > max) {
            // Empty range, return min
            return min;
        }

        return Mth::randomBetweenInclusive(random, min, max);
    }

protected:
    int32_t sampleImpl(void* random, const WorldGenerationContext& context) override {
        // Can't call template version without type info - return min as fallback
        return m_minInclusive.resolveY(context);
    }
};

/**
 * TrapezoidHeight - Returns height with trapezoid/triangle distribution
 * Reference: TrapezoidHeight.java
 *
 * Creates a trapezoidal probability distribution with a flat plateau
 * in the middle. When plateau=0, this is a triangular distribution.
 */
class TrapezoidHeight : public HeightProvider {
private:
    VerticalAnchor m_minInclusive;
    VerticalAnchor m_maxInclusive;
    int32_t m_plateau;

public:
    /**
     * Factory method with plateau
     * Reference: TrapezoidHeight.java lines 26-28
     */
    static TrapezoidHeight of(const VerticalAnchor& minInclusive,
                              const VerticalAnchor& maxInclusive,
                              int32_t plateau) {
        return TrapezoidHeight(minInclusive, maxInclusive, plateau);
    }

    /**
     * Factory method for triangle distribution (plateau=0)
     * Reference: TrapezoidHeight.java lines 30-32
     */
    static TrapezoidHeight of(const VerticalAnchor& minInclusive,
                              const VerticalAnchor& maxInclusive) {
        return of(minInclusive, maxInclusive, 0);
    }

    TrapezoidHeight(const VerticalAnchor& minInclusive,
                    const VerticalAnchor& maxInclusive,
                    int32_t plateau)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
        , m_plateau(plateau)
    {}

    /**
     * Sample - returns height with trapezoid distribution
     * Reference: TrapezoidHeight.java lines 34-49
     *
     * Algorithm:
     * 1. If plateau >= range, use uniform distribution
     * 2. Otherwise, sample two uniform values and add them (creates triangle)
     *    with adjustment for plateau width
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (min > max) {
            return min;
        }

        int32_t range = max - min;
        if (m_plateau >= range) {
            return Mth::randomBetweenInclusive(random, min, max);
        } else {
            // Create trapezoid distribution by summing two uniform samples
            // Reference: TrapezoidHeight.java lines 45-47
            int32_t plateauStart = (range - m_plateau) / 2;
            int32_t plateauEnd = range - plateauStart;
            return min + Mth::randomBetweenInclusive(random, 0, plateauEnd)
                       + Mth::randomBetweenInclusive(random, 0, plateauStart);
        }
    }

protected:
    int32_t sampleImpl(void* random, const WorldGenerationContext& context) override {
        return m_minInclusive.resolveY(context);
    }
};

/**
 * BiasedToBottomHeight - Height biased toward lower values
 * Reference: BiasedToBottomHeight.java
 */
class BiasedToBottomHeight : public HeightProvider {
private:
    VerticalAnchor m_minInclusive;
    VerticalAnchor m_maxInclusive;
    int32_t m_inner;

public:
    static BiasedToBottomHeight of(const VerticalAnchor& minInclusive,
                                   const VerticalAnchor& maxInclusive,
                                   int32_t inner) {
        return BiasedToBottomHeight(minInclusive, maxInclusive, inner);
    }

    BiasedToBottomHeight(const VerticalAnchor& minInclusive,
                         const VerticalAnchor& maxInclusive,
                         int32_t inner)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
        , m_inner(inner)
    {}

    /**
     * Sample with bias toward bottom
     * Reference: BiasedToBottomHeight.java lines 28-38
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (max - min - m_inner + 1 <= 0) {
            return min;
        }

        int32_t sample1 = random.nextInt(max - min - m_inner + 1);
        int32_t sample2 = random.nextInt(m_inner + 1);
        return min + sample1 + sample2;
    }

protected:
    int32_t sampleImpl(void* random, const WorldGenerationContext& context) override {
        return m_minInclusive.resolveY(context);
    }
};

/**
 * VeryBiasedToBottomHeight - Height very biased toward lower values
 * Reference: VeryBiasedToBottomHeight.java
 */
class VeryBiasedToBottomHeight : public HeightProvider {
private:
    VerticalAnchor m_minInclusive;
    VerticalAnchor m_maxInclusive;
    int32_t m_inner;

public:
    static VeryBiasedToBottomHeight of(const VerticalAnchor& minInclusive,
                                       const VerticalAnchor& maxInclusive,
                                       int32_t inner) {
        return VeryBiasedToBottomHeight(minInclusive, maxInclusive, inner);
    }

    VeryBiasedToBottomHeight(const VerticalAnchor& minInclusive,
                             const VerticalAnchor& maxInclusive,
                             int32_t inner)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
        , m_inner(inner)
    {}

    /**
     * Sample with very strong bias toward bottom using 3 nested Mth.nextInt calls
     * Reference: VeryBiasedToBottomHeight.java lines 37-39
     *
     * CRITICAL: Uses 3 nested Mth.nextInt calls, NOT float multiplication.
     * Each call narrows the range toward the bottom.
     *
     * Java code:
     *   int upperInclusive = Mth.nextInt(random, min + this.inner, max);
     *   int biasedUpperInclusive = Mth.nextInt(random, min, upperInclusive - 1);
     *   return Mth.nextInt(random, min, biasedUpperInclusive - 1 + this.inner);
     */
    template<typename RandomSource>
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        // Reference: VeryBiasedToBottomHeight.java lines 37-39
        // Call 1: upperInclusive in range [min + inner, max]
        int32_t upperInclusive = Mth::nextInt(random, min + m_inner, max);

        // Call 2: biasedUpperInclusive in range [min, upperInclusive - 1]
        int32_t biasedUpperInclusive = Mth::nextInt(random, min, upperInclusive - 1);

        // Call 3: final result in range [min, biasedUpperInclusive - 1 + inner]
        return Mth::nextInt(random, min, biasedUpperInclusive - 1 + m_inner);
    }

protected:
    int32_t sampleImpl(void* random, const WorldGenerationContext& context) override {
        return m_minInclusive.resolveY(context);
    }
};

/**
 * WeightedListHeight - Weighted random selection of height providers
 * Reference: WeightedListHeight.java
 */
template<typename RandomSource>
class WeightedListHeight : public HeightProvider {
public:
    struct WeightedEntry {
        std::shared_ptr<HeightProvider> provider;
        int weight;

        WeightedEntry(std::shared_ptr<HeightProvider> p, int w)
            : provider(p), weight(w) {}
    };

private:
    std::vector<WeightedEntry> m_distribution;
    int m_totalWeight;

public:
    explicit WeightedListHeight(const std::vector<WeightedEntry>& distribution)
        : m_distribution(distribution)
        , m_totalWeight(0)
    {
        for (const auto& entry : m_distribution) {
            m_totalWeight += entry.weight;
        }
    }

    /**
     * Sample - select random provider based on weights and sample from it
     * Reference: WeightedListHeight.java lines 17-18
     */
    int32_t sample(RandomSource& random, const WorldGenerationContext& context) {
        if (m_distribution.empty() || m_totalWeight <= 0) {
            return 0;
        }

        int randomWeight = random.nextInt(m_totalWeight);
        int currentWeight = 0;

        for (const auto& entry : m_distribution) {
            currentWeight += entry.weight;
            if (randomWeight < currentWeight) {
                return entry.provider->template sample<RandomSource>(random, context);
            }
        }

        // Fallback to last provider
        return m_distribution.back().provider->template sample<RandomSource>(random, context);
    }

protected:
    int32_t sampleImpl(void* random, const WorldGenerationContext& context) override {
        // Fallback for non-templated usage
        if (m_distribution.empty()) {
            return 0;
        }
        return m_distribution[0].provider->sampleImpl(random, context);
    }
};

} // namespace levelgen
} // namespace minecraft
