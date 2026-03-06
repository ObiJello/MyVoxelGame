#pragma once

#include "levelgen/SurfaceRules.h"
#include "levelgen/WorldgenRandom.h"
#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <set>
#include <string>

// Reference: net/minecraft/world/level/levelgen/carver/CarverConfiguration.java
// Reference: net/minecraft/util/valueproviders/FloatProvider.java
// Reference: net/minecraft/world/level/levelgen/heightproviders/HeightProvider.java

namespace minecraft {
namespace levelgen {
namespace carver {

//=============================================================================
// FloatProvider - Value providers for float sampling
// Reference: net/minecraft/util/valueproviders/FloatProvider.java
//=============================================================================

/**
 * FloatProvider - Abstract base for float value providers
 * Reference: FloatProvider.java
 */
class FloatProvider {
public:
    virtual ~FloatProvider() = default;
    virtual float sample(WorldgenRandom& random) const = 0;
    virtual float sample(LegacyRandomSource& random) const = 0;
    virtual float sample(XoroshiroRandomSource& random) const = 0;
    virtual float getMinValue() const = 0;
    virtual float getMaxValue() const = 0;
};

/**
 * ConstantFloat - Returns a constant float value
 * Reference: ConstantFloat.java
 */
class ConstantFloat : public FloatProvider {
private:
    float m_value;

public:
    explicit ConstantFloat(float value) : m_value(value) {}

    static ConstantFloat of(float value) {
        return ConstantFloat(value);
    }

    float sample(WorldgenRandom& random) const override {
        return m_value;
    }

    float sample(LegacyRandomSource& random) const override {
        return m_value;
    }

    float sample(XoroshiroRandomSource& random) const override {
        return m_value;
    }

    float getMinValue() const override { return m_value; }
    float getMaxValue() const override { return m_value; }
    float getValue() const { return m_value; }
};

/**
 * UniformFloat - Returns uniform random between min and max
 * Reference: UniformFloat.java
 */
class UniformFloat : public FloatProvider {
private:
    float m_minValue;
    float m_maxValue;

public:
    UniformFloat(float minValue, float maxValue)
        : m_minValue(minValue), m_maxValue(maxValue) {}

    static UniformFloat of(float minValue, float maxValue) {
        return UniformFloat(minValue, maxValue);
    }

    float sample(WorldgenRandom& random) const override {
        return random.nextFloat() * (m_maxValue - m_minValue) + m_minValue;
    }

    float sample(LegacyRandomSource& random) const override {
        return random.nextFloat() * (m_maxValue - m_minValue) + m_minValue;
    }

    float sample(XoroshiroRandomSource& random) const override {
        return random.nextFloat() * (m_maxValue - m_minValue) + m_minValue;
    }

    float getMinValue() const override { return m_minValue; }
    float getMaxValue() const override { return m_maxValue; }
};

/**
 * TrapezoidFloat - Returns float with trapezoid/triangle distribution
 * Reference: TrapezoidFloat.java
 *
 * Creates a trapezoidal probability distribution. The plateau parameter is
 * the WIDTH of the flat section in the middle, not a position.
 */
class TrapezoidFloat : public FloatProvider {
private:
    float m_min;
    float m_max;
    float m_plateau;  // Width of the flat section, not a position!

public:
    TrapezoidFloat(float min, float max, float plateau)
        : m_min(min), m_max(max), m_plateau(plateau) {}

    static TrapezoidFloat of(float min, float max, float plateau) {
        return TrapezoidFloat(min, max, plateau);
    }

    /**
     * Sample using trapezoid distribution
     * Reference: TrapezoidFloat.java lines 31-35
     */
    float sample(WorldgenRandom& random) const override {
        // Reference: TrapezoidFloat.java sample()
        float range = m_max - m_min;
        float plateauStart = (range - m_plateau) / 2.0f;
        float plateauEnd = range - plateauStart;
        return m_min + random.nextFloat() * plateauEnd + random.nextFloat() * plateauStart;
    }

    float sample(LegacyRandomSource& random) const override {
        // Reference: TrapezoidFloat.java sample()
        float range = m_max - m_min;
        float plateauStart = (range - m_plateau) / 2.0f;
        float plateauEnd = range - plateauStart;
        return m_min + random.nextFloat() * plateauEnd + random.nextFloat() * plateauStart;
    }

    float sample(XoroshiroRandomSource& random) const override {
        // Reference: TrapezoidFloat.java sample()
        float range = m_max - m_min;
        float plateauStart = (range - m_plateau) / 2.0f;
        float plateauEnd = range - plateauStart;
        return m_min + random.nextFloat() * plateauEnd + random.nextFloat() * plateauStart;
    }

    float getMinValue() const override { return m_min; }
    float getMaxValue() const override { return m_max; }
};

/**
 * ClampedNormalFloat - Returns float from clamped normal distribution
 * Reference: ClampedNormalFloat.java
 */
class ClampedNormalFloat : public FloatProvider {
private:
    float m_mean;
    float m_deviation;
    float m_min;
    float m_max;

public:
    ClampedNormalFloat(float mean, float deviation, float min, float max)
        : m_mean(mean), m_deviation(deviation), m_min(min), m_max(max) {}

    static ClampedNormalFloat of(float mean, float deviation, float min, float max) {
        return ClampedNormalFloat(mean, deviation, min, max);
    }

    // Reference: ClampedNormalFloat.java sample()
    // return Mth.clamp(mean + (float)random.nextGaussian() * deviation, min, max)
    static float sample(WorldgenRandom& random, float mean, float deviation, float min, float max) {
        float value = mean + static_cast<float>(random.nextGaussian()) * deviation;
        return std::clamp(value, min, max);
    }

    float sample(WorldgenRandom& random) const override {
        return sample(random, m_mean, m_deviation, m_min, m_max);
    }

    float sample(LegacyRandomSource& random) const override {
        // Fallback - shouldn't normally be called for dripstone
        return m_mean;
    }

    float sample(XoroshiroRandomSource& random) const override {
        // Fallback - shouldn't normally be called for dripstone
        return m_mean;
    }

    float getMinValue() const override { return m_min; }
    float getMaxValue() const override { return m_max; }
};

//=============================================================================
// IntProvider - Value providers for integer sampling
// Reference: net/minecraft/util/valueproviders/IntProvider.java
//=============================================================================

/**
 * IntProvider - Abstract base for integer value providers
 * Reference: IntProvider.java
 */
class IntProvider {
public:
    virtual ~IntProvider() = default;
    virtual int32_t sample(WorldgenRandom& random) const = 0;
    virtual int32_t sample(LegacyRandomSource& random) const = 0;
    virtual int32_t sample(XoroshiroRandomSource& random) const = 0;
    virtual int32_t getMinValue() const = 0;
    virtual int32_t getMaxValue() const = 0;
};

/**
 * ConstantInt - Returns a constant integer value
 * Reference: ConstantInt.java
 */
class ConstantInt : public IntProvider {
private:
    int32_t m_value;

public:
    explicit ConstantInt(int32_t value) : m_value(value) {}

    static ConstantInt of(int32_t value) {
        return ConstantInt(value);
    }

    int32_t sample(WorldgenRandom& random) const override {
        return m_value;
    }

    int32_t sample(LegacyRandomSource& random) const override {
        return m_value;
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        return m_value;
    }

    int32_t getMinValue() const override { return m_value; }
    int32_t getMaxValue() const override { return m_value; }
    int32_t getValue() const { return m_value; }
};

/**
 * UniformInt - Returns uniform random between min and max
 * Reference: UniformInt.java
 */
class UniformInt : public IntProvider {
private:
    int32_t m_minValue;
    int32_t m_maxValue;

public:
    UniformInt(int32_t minValue, int32_t maxValue)
        : m_minValue(minValue), m_maxValue(maxValue) {}

    static UniformInt of(int32_t minValue, int32_t maxValue) {
        return UniformInt(minValue, maxValue);
    }

    int32_t sample(WorldgenRandom& random) const override {
        if (m_maxValue <= m_minValue) return m_minValue;
        return random.nextInt(m_maxValue - m_minValue + 1) + m_minValue;
    }

    int32_t sample(LegacyRandomSource& random) const override {
        if (m_maxValue <= m_minValue) return m_minValue;
        return random.nextInt(m_maxValue - m_minValue + 1) + m_minValue;
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        if (m_maxValue <= m_minValue) return m_minValue;
        return random.nextInt(m_maxValue - m_minValue + 1) + m_minValue;
    }

    int32_t getMinValue() const override { return m_minValue; }
    int32_t getMaxValue() const override { return m_maxValue; }
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

    int32_t sample(WorldgenRandom& random) const override {
        int32_t value = m_source->sample(random);
        return std::max(m_minInclusive, std::min(m_maxInclusive, value));
    }

    int32_t sample(LegacyRandomSource& random) const override {
        int32_t value = m_source->sample(random);
        return std::max(m_minInclusive, std::min(m_maxInclusive, value));
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        int32_t value = m_source->sample(random);
        return std::max(m_minInclusive, std::min(m_maxInclusive, value));
    }

    int32_t getMinValue() const override {
        return std::max(m_minInclusive, m_source->getMinValue());
    }

    int32_t getMaxValue() const override {
        return std::min(m_maxInclusive, m_source->getMaxValue());
    }
};

/**
 * ClampedNormalInt - Samples from normal (Gaussian) distribution and clamps to range
 * Reference: ClampedNormalInt.java
 */
class ClampedNormalInt : public IntProvider {
private:
    float m_mean;
    float m_deviation;
    int32_t m_minInclusive;
    int32_t m_maxInclusive;

public:
    static ClampedNormalInt of(float mean, float deviation, int32_t minInclusive, int32_t maxInclusive) {
        return ClampedNormalInt(mean, deviation, minInclusive, maxInclusive);
    }

    ClampedNormalInt(float mean, float deviation, int32_t minInclusive, int32_t maxInclusive)
        : m_mean(mean)
        , m_deviation(deviation)
        , m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive)
    {}

    // Reference: ClampedNormalInt.java sample() and Mth.normal()
    // CRITICAL: Java casts nextGaussian() to float BEFORE multiplying by deviation.
    // Mth.normal() = mean + (float)random.nextGaussian() * deviation
    // Then: (int)Mth.clamp(normal, min, max)
    int32_t sample(WorldgenRandom& random) const override {
        float gaussianFloat = static_cast<float>(random.nextGaussian());
        float value = m_mean + gaussianFloat * m_deviation;
        return static_cast<int32_t>(std::clamp(value,
            static_cast<float>(m_minInclusive),
            static_cast<float>(m_maxInclusive)));
    }

    int32_t sample(LegacyRandomSource& random) const override {
        float gaussianFloat = static_cast<float>(random.nextGaussian());
        float value = m_mean + gaussianFloat * m_deviation;
        return static_cast<int32_t>(std::clamp(value,
            static_cast<float>(m_minInclusive),
            static_cast<float>(m_maxInclusive)));
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        float value = static_cast<float>(m_mean + random.nextGaussian() * m_deviation);
        return static_cast<int32_t>(std::clamp(value,
            static_cast<float>(m_minInclusive),
            static_cast<float>(m_maxInclusive)));
    }

    int32_t getMinValue() const override { return m_minInclusive; }
    int32_t getMaxValue() const override { return m_maxInclusive; }
};

/**
 * WeightedIntEntry - Entry in a weighted list with an IntProvider and weight
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
     */
    int32_t sample(WorldgenRandom& random) const override {
        if (m_totalWeight <= 0 || m_distribution.empty()) {
            return 0;
        }

        // Reference: WeightedList.java getRandomOrThrow() lines 71-77
        int32_t selection = random.nextInt(m_totalWeight);

        // Find the selected provider
        int32_t accumulated = 0;
        for (const auto& entry : m_distribution) {
            accumulated += entry.weight;
            if (selection < accumulated) {
                return entry.provider->sample(random);
            }
        }

        // Fallback to last entry
        return m_distribution.back().provider->sample(random);
    }

    int32_t sample(LegacyRandomSource& random) const override {
        if (m_totalWeight <= 0 || m_distribution.empty()) {
            return 0;
        }

        int32_t selection = random.nextInt(m_totalWeight);

        int32_t accumulated = 0;
        for (const auto& entry : m_distribution) {
            accumulated += entry.weight;
            if (selection < accumulated) {
                return entry.provider->sample(random);
            }
        }

        return m_distribution.back().provider->sample(random);
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        if (m_totalWeight <= 0 || m_distribution.empty()) {
            return 0;
        }

        int32_t selection = random.nextInt(m_totalWeight);

        int32_t accumulated = 0;
        for (const auto& entry : m_distribution) {
            accumulated += entry.weight;
            if (selection < accumulated) {
                return entry.provider->sample(random);
            }
        }

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

/**
 * CountExtraIntProvider - Returns base count, with `extra` added with probability `chance`
 * Reference: PlacementUtils.java countExtra()
 * This is used for tree placements like countExtra(10, 0.1, 1)
 */
class CountExtraIntProvider : public IntProvider {
private:
    int32_t m_baseValue;
    float m_chance;
    int32_t m_extraValue;

public:
    CountExtraIntProvider(int32_t baseValue, float chance, int32_t extraValue)
        : m_baseValue(baseValue), m_chance(chance), m_extraValue(extraValue) {}

    int32_t sample(WorldgenRandom& random) const override {
        // Reference: ExtraChanceDecoratorConfiguration.java
        // Returns base + extra if random < chance, else base
        return m_baseValue + (random.nextFloat() < m_chance ? m_extraValue : 0);
    }

    int32_t sample(LegacyRandomSource& random) const override {
        return m_baseValue + (random.nextFloat() < m_chance ? m_extraValue : 0);
    }

    int32_t sample(XoroshiroRandomSource& random) const override {
        return m_baseValue + (random.nextFloat() < m_chance ? m_extraValue : 0);
    }

    int32_t getMinValue() const override { return m_baseValue; }
    int32_t getMaxValue() const override { return m_baseValue + m_extraValue; }
};

//=============================================================================
// HeightProvider - Value providers for height sampling
// Reference: net/minecraft/world/level/levelgen/heightproviders/HeightProvider.java
//=============================================================================

/**
 * HeightProvider - Abstract base for height value providers
 * Reference: HeightProvider.java
 */
class HeightProvider {
public:
    virtual ~HeightProvider() = default;
    virtual int32_t sample(WorldgenRandom& random, const WorldGenerationContext& context) const = 0;
    virtual int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const = 0;
    virtual int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const = 0;
};

/**
 * ConstantHeight - Returns a constant height based on VerticalAnchor
 * Reference: ConstantHeight.java
 */
class ConstantHeight : public HeightProvider {
private:
    VerticalAnchor m_value;

public:
    explicit ConstantHeight(const VerticalAnchor& value) : m_value(value) {}

    static ConstantHeight of(const VerticalAnchor& value) {
        return ConstantHeight(value);
    }

    int32_t sample(WorldgenRandom& random, const WorldGenerationContext& context) const override {
        return m_value.resolveY(context);
    }

    int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const override {
        return m_value.resolveY(context);
    }

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
        return m_value.resolveY(context);
    }

    const VerticalAnchor& getValue() const { return m_value; }
};

/**
 * UniformHeight - Returns uniform random height between min and max anchors
 * Reference: UniformHeight.java
 */
class UniformHeight : public HeightProvider {
private:
    VerticalAnchor m_minInclusive;
    VerticalAnchor m_maxInclusive;

public:
    UniformHeight(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive)
        : m_minInclusive(minInclusive), m_maxInclusive(maxInclusive) {}

    static UniformHeight of(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive) {
        return UniformHeight(minInclusive, maxInclusive);
    }

    int32_t sample(WorldgenRandom& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);
        if (min > max) {
            return min;
        }
        return random.nextInt(max - min + 1) + min;
    }

    int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);
        if (min > max) {
            return min;
        }
        return random.nextInt(max - min + 1) + min;
    }

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);
        if (min > max) {
            return min;
        }
        return random.nextInt(max - min + 1) + min;
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
    TrapezoidHeight(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive, int32_t plateau = 0)
        : m_minInclusive(minInclusive), m_maxInclusive(maxInclusive), m_plateau(plateau) {}

    static TrapezoidHeight of(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive, int32_t plateau = 0) {
        return TrapezoidHeight(minInclusive, maxInclusive, plateau);
    }

    /**
     * Sample - returns height with trapezoid distribution
     * Reference: TrapezoidHeight.java lines 34-49
     */
    int32_t sample(WorldgenRandom& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (min > max) {
            return min;
        }

        int32_t range = max - min;
        if (m_plateau >= range) {
            return random.nextInt(range + 1) + min;
        } else {
            // Create trapezoid distribution by summing two uniform samples
            // Reference: TrapezoidHeight.java lines 45-47
            int32_t plateauStart = (range - m_plateau) / 2;
            int32_t plateauEnd = range - plateauStart;
            return min + random.nextInt(plateauEnd + 1) + random.nextInt(plateauStart + 1);
        }
    }

    int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (min > max) {
            return min;
        }

        int32_t range = max - min;
        if (m_plateau >= range) {
            return random.nextInt(range + 1) + min;
        } else {
            // Create trapezoid distribution by summing two uniform samples
            // Reference: TrapezoidHeight.java lines 45-47
            int32_t plateauStart = (range - m_plateau) / 2;
            int32_t plateauEnd = range - plateauStart;
            return min + random.nextInt(plateauEnd + 1) + random.nextInt(plateauStart + 1);
        }
    }

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (min > max) {
            return min;
        }

        int32_t range = max - min;
        if (m_plateau >= range) {
            return random.nextInt(range + 1) + min;
        } else {
            // Create trapezoid distribution by summing two uniform samples
            // Reference: TrapezoidHeight.java lines 45-47
            int32_t plateauStart = (range - m_plateau) / 2;
            int32_t plateauEnd = range - plateauStart;
            return min + random.nextInt(plateauEnd + 1) + random.nextInt(plateauStart + 1);
        }
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
    BiasedToBottomHeight(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive, int32_t inner)
        : m_minInclusive(minInclusive), m_maxInclusive(maxInclusive), m_inner(inner) {}

    static BiasedToBottomHeight of(const VerticalAnchor& minInclusive, const VerticalAnchor& maxInclusive, int32_t inner) {
        return BiasedToBottomHeight(minInclusive, maxInclusive, inner);
    }

    /**
     * Sample with bias toward bottom
     * Reference: BiasedToBottomHeight.java lines 28-38
     */
    int32_t sample(WorldgenRandom& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (max - min - m_inner + 1 <= 0) {
            return min;
        }

        int32_t sample1 = random.nextInt(max - min - m_inner + 1);
        int32_t sample2 = random.nextInt(m_inner + 1);
        return min + sample1 + sample2;
    }

    int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (max - min - m_inner + 1 <= 0) {
            return min;
        }

        int32_t sample1 = random.nextInt(max - min - m_inner + 1);
        int32_t sample2 = random.nextInt(m_inner + 1);
        return min + sample1 + sample2;
    }

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
        int32_t min = m_minInclusive.resolveY(context);
        int32_t max = m_maxInclusive.resolveY(context);

        if (max - min - m_inner + 1 <= 0) {
            return min;
        }

        int32_t sample1 = random.nextInt(max - min - m_inner + 1);
        int32_t sample2 = random.nextInt(m_inner + 1);
        return min + sample1 + sample2;
    }
};

//=============================================================================
// CarverDebugSettings
// Reference: net/minecraft/world/level/levelgen/carver/CarverDebugSettings.java
//=============================================================================

class CarverDebugSettings {
private:
    bool m_debugMode;

public:
    CarverDebugSettings(bool debugMode = false) : m_debugMode(debugMode) {}

    bool isDebugMode() const { return m_debugMode; }

    static const CarverDebugSettings DEFAULT;
};

//=============================================================================
// CarverConfiguration - Base configuration for carvers
// Reference: net/minecraft/world/level/levelgen/carver/CarverConfiguration.java
//=============================================================================

/**
 * CarverConfiguration - Base carver configuration
 * Reference: CarverConfiguration.java
 */
class CarverConfiguration {
public:
    float probability;
    const HeightProvider* y;
    const FloatProvider* yScale;
    VerticalAnchor lavaLevel;
    CarverDebugSettings debugSettings;
    std::set<std::string> replaceable;  // Block names that can be replaced

    CarverConfiguration(
        float probability,
        const HeightProvider* y,
        const FloatProvider* yScale,
        const VerticalAnchor& lavaLevel,
        const CarverDebugSettings& debugSettings,
        const std::set<std::string>& replaceable
    )
        : probability(probability)
        , y(y)
        , yScale(yScale)
        , lavaLevel(lavaLevel)
        , debugSettings(debugSettings)
        , replaceable(replaceable)
    {}

    virtual ~CarverConfiguration() = default;
};

//=============================================================================
// CaveCarverConfiguration
// Reference: net/minecraft/world/level/levelgen/carver/CaveCarverConfiguration.java
//=============================================================================

/**
 * CaveCarverConfiguration - Configuration for cave carvers
 * Reference: CaveCarverConfiguration.java
 */
class CaveCarverConfiguration : public CarverConfiguration {
public:
    const FloatProvider* horizontalRadiusMultiplier;
    const FloatProvider* verticalRadiusMultiplier;
    const FloatProvider* floorLevel;

    CaveCarverConfiguration(
        float probability,
        const HeightProvider* y,
        const FloatProvider* yScale,
        const VerticalAnchor& lavaLevel,
        const CarverDebugSettings& debugSettings,
        const std::set<std::string>& replaceable,
        const FloatProvider* horizontalRadiusMultiplier,
        const FloatProvider* verticalRadiusMultiplier,
        const FloatProvider* floorLevel
    )
        : CarverConfiguration(probability, y, yScale, lavaLevel, debugSettings, replaceable)
        , horizontalRadiusMultiplier(horizontalRadiusMultiplier)
        , verticalRadiusMultiplier(verticalRadiusMultiplier)
        , floorLevel(floorLevel)
    {}
};

//=============================================================================
// CanyonShapeConfiguration
// Reference: net/minecraft/world/level/levelgen/carver/CanyonCarverConfiguration.CanyonShapeConfiguration
//=============================================================================

/**
 * CanyonShapeConfiguration - Shape parameters for canyons
 * Reference: CanyonCarverConfiguration.java lines 27-44
 */
class CanyonShapeConfiguration {
public:
    const FloatProvider* distanceFactor;
    const FloatProvider* thickness;
    int32_t widthSmoothness;
    const FloatProvider* horizontalRadiusFactor;
    float verticalRadiusDefaultFactor;
    float verticalRadiusCenterFactor;

    CanyonShapeConfiguration(
        const FloatProvider* distanceFactor,
        const FloatProvider* thickness,
        int32_t widthSmoothness,
        const FloatProvider* horizontalRadiusFactor,
        float verticalRadiusDefaultFactor,
        float verticalRadiusCenterFactor
    )
        : distanceFactor(distanceFactor)
        , thickness(thickness)
        , widthSmoothness(widthSmoothness)
        , horizontalRadiusFactor(horizontalRadiusFactor)
        , verticalRadiusDefaultFactor(verticalRadiusDefaultFactor)
        , verticalRadiusCenterFactor(verticalRadiusCenterFactor)
    {}
};

//=============================================================================
// CanyonCarverConfiguration
// Reference: net/minecraft/world/level/levelgen/carver/CanyonCarverConfiguration.java
//=============================================================================

/**
 * CanyonCarverConfiguration - Configuration for canyon carvers
 * Reference: CanyonCarverConfiguration.java
 */
class CanyonCarverConfiguration : public CarverConfiguration {
public:
    const FloatProvider* verticalRotation;
    CanyonShapeConfiguration shape;

    CanyonCarverConfiguration(
        float probability,
        const HeightProvider* y,
        const FloatProvider* yScale,
        const VerticalAnchor& lavaLevel,
        const CarverDebugSettings& debugSettings,
        const std::set<std::string>& replaceable,
        const FloatProvider* verticalRotation,
        const CanyonShapeConfiguration& shape
    )
        : CarverConfiguration(probability, y, yScale, lavaLevel, debugSettings, replaceable)
        , verticalRotation(verticalRotation)
        , shape(shape)
    {}
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
