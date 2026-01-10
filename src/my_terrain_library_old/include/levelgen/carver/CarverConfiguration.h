#pragma once

#include "levelgen/SurfaceRules.h"
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
    virtual float sample(XoroshiroRandomSource& random) const = 0;
    virtual float sample(LegacyRandomSource& random) const = 0;
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

    float sample(XoroshiroRandomSource& random) const override {
        return m_value;
    }

    float sample(LegacyRandomSource& random) const override {
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

    float sample(XoroshiroRandomSource& random) const override {
        return random.nextFloat() * (m_maxValue - m_minValue) + m_minValue;
    }

    float sample(LegacyRandomSource& random) const override {
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
    float sample(XoroshiroRandomSource& random) const override {
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
    virtual int32_t sample(XoroshiroRandomSource& random) const = 0;
    virtual int32_t sample(LegacyRandomSource& random) const = 0;
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

    int32_t sample(XoroshiroRandomSource& random) const override {
        return m_value;
    }

    int32_t sample(LegacyRandomSource& random) const override {
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

    int32_t sample(XoroshiroRandomSource& random) const override {
        if (m_maxValue <= m_minValue) return m_minValue;
        return random.nextInt(m_maxValue - m_minValue + 1) + m_minValue;
    }

    int32_t sample(LegacyRandomSource& random) const override {
        if (m_maxValue <= m_minValue) return m_minValue;
        return random.nextInt(m_maxValue - m_minValue + 1) + m_minValue;
    }

    int32_t getMinValue() const override { return m_minValue; }
    int32_t getMaxValue() const override { return m_maxValue; }
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
    virtual int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const = 0;
    virtual int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const = 0;
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

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
        return m_value.resolveY(context);
    }

    int32_t sample(LegacyRandomSource& random, const WorldGenerationContext& context) const override {
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

    int32_t sample(XoroshiroRandomSource& random, const WorldGenerationContext& context) const override {
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
