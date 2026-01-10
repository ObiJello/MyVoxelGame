#pragma once

#include "math/Mth.h"
#include "random/XoroshiroRandomSource.h"
#include <cstdint>
#include <algorithm>

// Reference: net/minecraft/util/valueproviders/IntProvider.java
// Reference: net/minecraft/util/valueproviders/ConstantInt.java
// Reference: net/minecraft/util/valueproviders/UniformInt.java
// Reference: net/minecraft/util/valueproviders/ClampedInt.java
// Reference: net/minecraft/util/valueproviders/BiasedToBottomInt.java

namespace minecraft {
namespace util {

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
    virtual int32_t sample(XoroshiroRandomSource& random) = 0;

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
    int32_t sample(XoroshiroRandomSource& /*random*/) override {
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
    int32_t sample(XoroshiroRandomSource& random) override {
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

    int32_t sample(XoroshiroRandomSource& random) override {
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
     * Sample with bias toward lower values
     * Reference: BiasedToBottomInt.java lines 22-26
     */
    int32_t sample(XoroshiroRandomSource& random) override {
        // Uses minimum of two uniform samples to bias toward bottom
        int32_t range = m_maxInclusive - m_minInclusive + 1;
        int32_t first = random.nextInt(range);
        int32_t second = random.nextInt(range);
        return std::min(first, second) + m_minInclusive;
    }
};

} // namespace util
} // namespace minecraft
