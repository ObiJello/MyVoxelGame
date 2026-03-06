#pragma once

#include "math/Mth.h"
#include "levelgen/WorldgenRandom.h"
#include <cstdint>
#include <algorithm>
#include <vector>
#include <memory>
#include <climits>

// Reference: net/minecraft/util/valueproviders/IntProvider.java
// Reference: net/minecraft/util/valueproviders/ConstantInt.java
// Reference: net/minecraft/util/valueproviders/UniformInt.java
// Reference: net/minecraft/util/valueproviders/ClampedInt.java
// Reference: net/minecraft/util/valueproviders/BiasedToBottomInt.java

namespace minecraft {
namespace util {

// Use WorldgenRandom from levelgen namespace
using levelgen::WorldgenRandom;

/**
 * IntProvider - Abstract base class for integer value providers
 * Reference: IntProvider.java
 */
class IntProvider {
public:
    virtual ~IntProvider() = default;

    /**
     * Sample a value from this provider
     * Reference: IntProvider.java line 31
     */
    virtual int32_t sample(WorldgenRandom& random) = 0;

    /**
     * Get the minimum possible value
     * Reference: IntProvider.java line 33
     */
    virtual int32_t getMinValue() const = 0;

    /**
     * Get the maximum possible value
     * Reference: IntProvider.java line 35
     */
    virtual int32_t getMaxValue() const = 0;
};

/**
 * ConstantInt - Returns a constant value
 * Reference: ConstantInt.java
 */
class ConstantInt : public IntProvider {
private:
    int32_t m_value;

public:
    /**
     * Factory method
     * Reference: ConstantInt.java lines 12-14
     */
    static ConstantInt of(int32_t value) {
        return ConstantInt(value);
    }

    explicit ConstantInt(int32_t value) : m_value(value) {}

    int32_t getValue() const { return m_value; }

    int32_t getMinValue() const override { return m_value; }
    int32_t getMaxValue() const override { return m_value; }

    /**
     * Sample - always returns constant value
     * Reference: ConstantInt.java lines 24-26
     */
    int32_t sample(WorldgenRandom& /*random*/) override {
        return m_value;
    }
};

/**
 * UniformInt - Returns uniform random value in range [min, max]
 * Reference: UniformInt.java
 */
class UniformInt : public IntProvider {
private:
    int32_t m_minInclusive;
    int32_t m_maxInclusive;

public:
    /**
     * Factory method
     * Reference: UniformInt.java lines 20-22
     */
    static UniformInt of(int32_t minInclusive, int32_t maxInclusive) {
        return UniformInt(minInclusive, maxInclusive);
    }

    UniformInt(int32_t minInclusive, int32_t maxInclusive)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
    {}

    int32_t getMinValue() const override { return m_minInclusive; }
    int32_t getMaxValue() const override { return m_maxInclusive; }

    /**
     * Sample - returns uniform random value in range
     * Reference: UniformInt.java lines 24-26
     */
    int32_t sample(WorldgenRandom& random) override {
        return Mth::randomBetweenInclusive(random, m_minInclusive, m_maxInclusive);
    }
};

/**
 * ClampedInt - Clamps another provider's output to a range
 * Reference: ClampedInt.java
 */
class ClampedInt : public IntProvider {
private:
    IntProvider* m_source;
    int32_t m_minInclusive;
    int32_t m_maxInclusive;

public:
    static ClampedInt of(IntProvider* source, int32_t minInclusive, int32_t maxInclusive) {
        return ClampedInt(source, minInclusive, maxInclusive);
    }

    ClampedInt(IntProvider* source, int32_t minInclusive, int32_t maxInclusive)
        : m_source(source)
        , m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
    {}

    int32_t getMinValue() const override {
        return std::max(m_minInclusive, m_source->getMinValue());
    }

    int32_t getMaxValue() const override {
        return std::min(m_maxInclusive, m_source->getMaxValue());
    }

    int32_t sample(WorldgenRandom& random) override {
        return Mth::clamp(m_source->sample(random), m_minInclusive, m_maxInclusive);
    }
};

/**
 * BiasedToBottomInt - Biased toward lower values
 * Reference: BiasedToBottomInt.java
 */
class BiasedToBottomInt : public IntProvider {
private:
    int32_t m_minInclusive;
    int32_t m_maxInclusive;

public:
    static BiasedToBottomInt of(int32_t minInclusive, int32_t maxInclusive) {
        return BiasedToBottomInt(minInclusive, maxInclusive);
    }

    BiasedToBottomInt(int32_t minInclusive, int32_t maxInclusive)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
    {}

    int32_t getMinValue() const override { return m_minInclusive; }
    int32_t getMaxValue() const override { return m_maxInclusive; }

    /**
     * Sample with bias toward lower values using NESTED dependent random
     * Reference: BiasedToBottomInt.java lines 23-25
     *
     * CRITICAL: Uses nested dependent calls: min + nextInt(nextInt(range) + 1)
     * The inner nextInt determines the upper bound for the outer nextInt.
     * This is NOT the same as min(nextInt(range), nextInt(range))!
     */
    int32_t sample(WorldgenRandom& random) override {
        // Reference: BiasedToBottomInt.java line 24
        // return this.minInclusive + random.nextInt(random.nextInt(this.maxInclusive - this.minInclusive + 1) + 1);
        int32_t range = m_maxInclusive - m_minInclusive + 1;

        // First call: determines upper bound for second call
        int32_t innerResult = random.nextInt(range);  // [0, range)

        // Second call: uses inner result + 1 as upper bound
        int32_t outerResult = random.nextInt(innerResult + 1);  // [0, innerResult]

        return m_minInclusive + outerResult;
    }
};

/**
 * WeightedEntry - Entry in a weighted list with an IntProvider and weight
 * Reference: net/minecraft/util/random/Weighted.java
 */
struct WeightedIntEntry {
    std::shared_ptr<IntProvider> provider;
    int32_t weight;

    WeightedIntEntry(std::shared_ptr<IntProvider> p, int32_t w)
        : provider(std::move(p)), weight(w) {}
};

/**
 * WeightedListInt - Weighted distribution of IntProviders
 * Reference: WeightedListInt.java
 *
 * Selects a random IntProvider from a weighted distribution, then samples from it.
 */
class WeightedListInt : public IntProvider {
private:
    std::vector<WeightedIntEntry> m_distribution;
    int32_t m_totalWeight;
    int32_t m_minValue;
    int32_t m_maxValue;

public:
    /**
     * Constructor
     * Reference: WeightedListInt.java lines 15-29
     */
    explicit WeightedListInt(const std::vector<WeightedIntEntry>& distribution)
        : m_distribution(distribution)
        , m_totalWeight(0)
        , m_minValue(INT32_MAX)
        , m_maxValue(INT32_MIN)
    {
        // Calculate total weight and min/max values
        for (const auto& entry : m_distribution) {
            m_totalWeight += entry.weight;
            if (entry.provider) {
                m_minValue = std::min(m_minValue, entry.provider->getMinValue());
                m_maxValue = std::max(m_maxValue, entry.provider->getMaxValue());
            }
        }
        // Handle empty distribution
        if (m_distribution.empty()) {
            m_minValue = 0;
            m_maxValue = 0;
        }
    }

    int32_t getMinValue() const override { return m_minValue; }
    int32_t getMaxValue() const override { return m_maxValue; }

    /**
     * Sample from weighted distribution
     * Reference: WeightedListInt.java lines 31-33
     *
     * 1. Select a random provider from the distribution (consumes 1 random call)
     * 2. Sample from that provider (consumes provider's random calls)
     */
    int32_t sample(WorldgenRandom& random) override {
        if (m_totalWeight <= 0 || m_distribution.empty()) {
            return 0;
        }

        // Reference: WeightedList.java getRandomOrThrow() lines 71-77
        // int selection = random.nextInt(this.totalWeight);
        int32_t selection = random.nextInt(m_totalWeight);

        // Find the selected provider
        int32_t accumulated = 0;
        for (const auto& entry : m_distribution) {
            accumulated += entry.weight;
            if (selection < accumulated) {
                // Reference: WeightedListInt.java line 32
                // return ((IntProvider)this.distribution.getRandomOrThrow(random)).sample(random);
                return entry.provider->sample(random);
            }
        }

        // Fallback to last entry
        return m_distribution.back().provider->sample(random);
    }

    /**
     * Builder helper for creating weighted distributions
     */
    class Builder {
    private:
        std::vector<WeightedIntEntry> m_entries;

    public:
        Builder& add(std::shared_ptr<IntProvider> provider, int32_t weight) {
            m_entries.emplace_back(std::move(provider), weight);
            return *this;
        }

        WeightedListInt build() {
            return WeightedListInt(m_entries);
        }

        std::shared_ptr<WeightedListInt> buildShared() {
            return std::make_shared<WeightedListInt>(m_entries);
        }
    };

    static Builder builder() {
        return Builder();
    }
};

} // namespace util
} // namespace minecraft
