#pragma once

#include "levelgen/DensityFunction.h"
#include "levelgen/Blender.h"
#include "synth/NormalNoise.h"
#include "synth/SimplexNoise.h"
#include "math/Mth.h"
#include "util/CubicSpline.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace minecraft {
namespace density {

/**
 * DensityFunctions - Concrete implementations of DensityFunction
 *
 * This is the collection of all standard density functions used in terrain generation.
 * Reference: net/minecraft/world/level/levelgen/DensityFunctions.java
 */
namespace DensityFunctions {

// Forward declarations
class Constant;
class Noise;
class ShiftedNoise;
class BlendAlpha;
class BlendOffset;
class BlendDensity;
class Clamp;
class Mapped;
class MulOrAdd;
class Ap2;
class YClampedGradient;
class ShiftA;
class ShiftB;
class Shift;
class WeirdScaledSampler;

// Two-argument function types (forward declaration)
enum class TwoArgType {
    ADD,  // add
    MUL,  // mul
    MIN,  // min
    MAX   // max
};

// Forward declaration of factory function
DensityFunction* createTwoArgumentFunction(TwoArgType type, DensityFunction* arg1, DensityFunction* arg2);

// ============================================================================
// CONSTANT FUNCTION
// ============================================================================

/**
 * Constant - Always returns the same value regardless of position
 * Java: DensityFunctions.Constant (line 1192)
 */
class Constant : public SimpleFunction {
public:
    explicit Constant(double value) : m_value(value) {}

    double compute(const FunctionContext& context) const override {
        (void)context;
        return m_value;
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        (void)contextProvider;
        // Arrays.fill(output, this.value)
        for (int32_t i = 0; i < count; ++i) {
            output[i] = m_value;
        }
    }

    double minValue() const override { return m_value; }
    double maxValue() const override { return m_value; }

    double value() const { return m_value; }

    // Static ZERO constant (Java line 1218)
    static Constant* ZERO();

private:
    double m_value;
    static Constant* s_zero;
};

// ============================================================================
// NOISE FUNCTION
// ============================================================================

/**
 * Noise - Wraps a NoiseHolder and samples it with scaling
 * Java: DensityFunctions.Noise (line 467)
 */
class Noise : public DensityFunction {
public:
    Noise(DensityFunction::NoiseHolder* noise, double xzScale, double yScale)
        : m_noise(noise), m_xzScale(xzScale), m_yScale(yScale) {}

    double compute(const FunctionContext& context) const override {
        // Java line 478: this.noise.getValue((double)context.blockX() * this.xzScale, ...)
        return m_noise->getValue(
            static_cast<double>(context.blockX()) * m_xzScale,
            static_cast<double>(context.blockY()) * m_yScale,
            static_cast<double>(context.blockZ()) * m_xzScale
        );
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 482: contextProvider.fillAllDirectly(output, this)
        contextProvider.fillAllDirectly(output, count, const_cast<Noise*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 486: visitor.apply(new Noise(visitor.visitNoise(this.noise), ...))
        DensityFunction::NoiseHolder* visitedNoise = visitor.visitNoise(m_noise);
        return visitor.apply(new Noise(visitedNoise, m_xzScale, m_yScale));
    }

    double minValue() const override {
        // Java line 490: -this.maxValue()
        return -maxValue();
    }

    double maxValue() const override {
        // Java line 494: this.noise.maxValue()
        return m_noise->maxValue();
    }

    DensityFunction::NoiseHolder* noise() const { return m_noise; }
    double xzScale() const { return m_xzScale; }
    double yScale() const { return m_yScale; }

private:
    DensityFunction::NoiseHolder* m_noise;
    double m_xzScale;
    double m_yScale;
};

// ============================================================================
// SHIFTED NOISE FUNCTION
// ============================================================================

/**
 * ShiftedNoise - Noise with coordinate shifting
 * Java: DensityFunctions.ShiftedNoise (lines 621-655)
 *
 * Shifts the sampling coordinates before reading from noise:
 * - x = blockX * xzScale + shiftX.compute(context)
 * - y = blockY * yScale + shiftY.compute(context)
 * - z = blockZ * xzScale + shiftZ.compute(context)
 */
class ShiftedNoise : public DensityFunction {
public:
    ShiftedNoise(DensityFunction* shiftX, DensityFunction* shiftY, DensityFunction* shiftZ,
                 double xzScale, double yScale, DensityFunction::NoiseHolder* noise)
        : m_shiftX(shiftX), m_shiftY(shiftY), m_shiftZ(shiftZ)
        , m_xzScale(xzScale), m_yScale(yScale), m_noise(noise) {}

    double compute(const FunctionContext& context) const override {
        // Java lines 625-630
        double x = static_cast<double>(context.blockX()) * m_xzScale + m_shiftX->compute(context);
        double y = static_cast<double>(context.blockY()) * m_yScale + m_shiftY->compute(context);
        double z = static_cast<double>(context.blockZ()) * m_xzScale + m_shiftZ->compute(context);
        return m_noise->getValue(x, y, z);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 633
        contextProvider.fillAllDirectly(output, count, const_cast<ShiftedNoise*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 637
        return visitor.apply(new ShiftedNoise(
            m_shiftX->mapAll(visitor),
            m_shiftY->mapAll(visitor),
            m_shiftZ->mapAll(visitor),
            m_xzScale,
            m_yScale,
            visitor.visitNoise(m_noise)
        ));
    }

    double minValue() const override {
        // Java line 641
        return -maxValue();
    }

    double maxValue() const override {
        // Java line 645
        return m_noise->maxValue();
    }

    // Accessors
    DensityFunction* shiftX() const { return m_shiftX; }
    DensityFunction* shiftY() const { return m_shiftY; }
    DensityFunction* shiftZ() const { return m_shiftZ; }
    double xzScale() const { return m_xzScale; }
    double yScale() const { return m_yScale; }
    DensityFunction::NoiseHolder* noise() const { return m_noise; }

private:
    DensityFunction* m_shiftX;
    DensityFunction* m_shiftY;
    DensityFunction* m_shiftZ;
    double m_xzScale;
    double m_yScale;
    DensityFunction::NoiseHolder* m_noise;
};

// ============================================================================
// BLENDER FUNCTIONS
// ============================================================================

/**
 * BlendAlpha - Always returns 1.0 (singleton)
 * Java: DensityFunctions.BlendAlpha (lines 289-318)
 *
 * This is a placeholder for terrain blending alpha value.
 * Always returns 1.0, meaning no blending in default implementation.
 */
class BlendAlpha : public SimpleFunction {
public:
    // Singleton instance
    static BlendAlpha* instance() {
        static BlendAlpha s_instance;
        return &s_instance;
    }

    // Fast type checking for wrapNew()
    WrapType getWrapType() const override { return WrapType::BlendAlpha; }

    double compute(const FunctionContext& context) const override {
        // Java line 295: return (double)1.0F
        return 1.0;
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 299: Arrays.fill(output, (double)1.0F)
        for (int32_t i = 0; i < count; ++i) {
            output[i] = 1.0;
        }
    }

    double minValue() const override {
        // Java line 303
        return 1.0;
    }

    double maxValue() const override {
        // Java line 307
        return 1.0;
    }

private:
    BlendAlpha() = default;  // Private constructor for singleton
};

/**
 * BlendOffset - Always returns 0.0 (singleton)
 * Java: DensityFunctions.BlendOffset (lines 320-349)
 *
 * This is a placeholder for terrain blending offset value.
 * Always returns 0.0, meaning no offset in default implementation.
 */
class BlendOffset : public SimpleFunction {
public:
    // Singleton instance
    static BlendOffset* instance() {
        static BlendOffset s_instance;
        return &s_instance;
    }

    // Fast type checking for wrapNew()
    WrapType getWrapType() const override { return WrapType::BlendOffset; }

    double compute(const FunctionContext& context) const override {
        // Java line 326: return (double)0.0F
        return 0.0;
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 330: Arrays.fill(output, (double)0.0F)
        for (int32_t i = 0; i < count; ++i) {
            output[i] = 0.0;
        }
    }

    double minValue() const override {
        // Java line 334
        return 0.0;
    }

    double maxValue() const override {
        // Java line 338
        return 0.0;
    }

private:
    BlendOffset() = default;  // Private constructor for singleton
};

/**
 * BlendDensity - Delegates to Blender.blendDensity()
 * Java: DensityFunctions.BlendDensity (lines 781-803)
 *
 * Applies terrain blending to density values at chunk boundaries.
 * With empty blender: just returns input unchanged.
 */
class BlendDensity : public DensityFunction {
public:
    BlendDensity(DensityFunction* input) : m_input(input) {}

    double compute(const FunctionContext& context) const override {
        // Java lines 784-786
        // First compute the input
        double inputValue = m_input->compute(context);
        // Then blend it using the context's blender (if available)
        Blender* blender = context.getBlender();
        if (blender == nullptr) {
            // No blender available - return input unchanged (empty blender behavior)
            return inputValue;
        }
        return blender->blendDensity(context, inputValue);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // TransformerWithContext pattern - use context-aware transformation
        // First fill with input values
        m_input->fillArray(output, count, contextProvider);
        // Then blend each value
        for (int32_t i = 0; i < count; ++i) {
            FunctionContext* ctx = contextProvider.forIndex(i);
            Blender* blender = ctx->getBlender();
            if (blender != nullptr) {
                output[i] = blender->blendDensity(*ctx, output[i]);
            }
            // If no blender, leave output unchanged
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 789
        return visitor.apply(new BlendDensity(m_input->mapAll(visitor)));
    }

    double minValue() const override {
        // Java line 793: blending can produce any value
        return -std::numeric_limits<double>::infinity();
    }

    double maxValue() const override {
        // Java line 797: blending can produce any value
        return std::numeric_limits<double>::infinity();
    }

    DensityFunction* input() const { return m_input; }

private:
    DensityFunction* m_input;
};

// ============================================================================
// CLAMP FUNCTION
// ============================================================================

/**
 * Clamp - Clamps input values between min and max
 * Java: DensityFunctions.Clamp (line 805)
 */
class Clamp : public SimpleFunction {
public:
    Clamp(DensityFunction* input, double minValue, double maxValue)
        : m_input(input), m_minValue(minValue), m_maxValue(maxValue) {}

    double compute(const FunctionContext& context) const override {
        // PureTransformer pattern: transform(this.input().compute(context))
        double value = m_input->compute(context);
        return transform(value);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java lines 277-283: fill array first, then transform each element
        m_input->fillArray(output, count, contextProvider);
        // SIMD hint for vectorization
        #pragma omp simd
        for (int32_t i = 0; i < count; ++i) {
            output[i] = Mth::clamp(output[i], m_minValue, m_maxValue);
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 814: new Clamp(this.input.mapAll(visitor), this.minValue, this.maxValue)
        return new Clamp(m_input->mapAll(visitor), m_minValue, m_maxValue);
    }

    double minValue() const override { return m_minValue; }
    double maxValue() const override { return m_maxValue; }

    double transform(double input) const {
        // Java line 810: Mth.clamp(input, this.minValue, this.maxValue)
        return Mth::clamp(input, m_minValue, m_maxValue);
    }

    DensityFunction* input() const { return m_input; }

private:
    DensityFunction* m_input;
    double m_minValue;
    double m_maxValue;
};

// ============================================================================
// MAPPED TRANSFORMATIONS
// ============================================================================

/**
 * Mapped - Applies mathematical transformations (abs, square, cube, etc.)
 * Java: DensityFunctions.Mapped (line 826)
 */
class Mapped : public SimpleFunction {
public:
    enum class Type {
        ABS,             // abs
        SQUARE,          // square
        CUBE,            // cube
        HALF_NEGATIVE,   // half_negative
        QUARTER_NEGATIVE,// quarter_negative
        INVERT,          // invert
        SQUEEZE          // squeeze
    };

    Mapped(Type type, DensityFunction* input, double minValue, double maxValue)
        : m_type(type), m_input(input), m_minValue(minValue), m_maxValue(maxValue) {}

    // Factory method with automatic min/max calculation
    // Java line 827: create(Type, DensityFunction)
    static Mapped* create(Type type, DensityFunction* input) {
        double minValue = input->minValue();
        double maxValue = input->maxValue();
        double minImage = transform(type, minValue);
        double maxImage = transform(type, maxValue);

        // Java lines 832-836
        if (type == Type::INVERT) {
            if (minValue < 0.0 && maxValue > 0.0) {
                return new Mapped(type, input, -INFINITY, INFINITY);
            } else {
                return new Mapped(type, input, maxImage, minImage);
            }
        } else if (type == Type::ABS || type == Type::SQUARE) {
            return new Mapped(type, input, std::max(0.0, minValue), std::max(minImage, maxImage));
        } else {
            return new Mapped(type, input, minImage, maxImage);
        }
    }

    double compute(const FunctionContext& context) const override {
        double value = m_input->compute(context);
        return transform(m_type, value);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        m_input->fillArray(output, count, contextProvider);
        // Note: transform() has switch, so SIMD may not fully vectorize
        // but compiler can still optimize scalar operations
        Type type = m_type;
        #pragma omp simd
        for (int32_t i = 0; i < count; ++i) {
            output[i] = transform(type, output[i]);
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 876: create(this.type, this.input.mapAll(visitor))
        return create(m_type, m_input->mapAll(visitor));
    }

    double minValue() const override { return m_minValue; }
    double maxValue() const override { return m_maxValue; }

    // Static transform function (Java line 839)
    static double transform(Type type, double input) {
        switch (type) {
            case Type::ABS:  // case 0
                return std::abs(input);

            case Type::SQUARE:  // case 1
                return input * input;

            case Type::CUBE:  // case 2
                return input * input * input;

            case Type::HALF_NEGATIVE:  // case 3
                return input > 0.0 ? input : input * 0.5;

            case Type::QUARTER_NEGATIVE:  // case 4
                return input > 0.0 ? input : input * 0.25;

            case Type::INVERT:  // case 5
                return 1.0 / input;

            case Type::SQUEEZE:  // case 6
                {
                    double c = Mth::clamp(input, -1.0, 1.0);
                    return c / 2.0 - c * c * c / 24.0;
                }

            default:
                throw std::runtime_error("Unknown Mapped type");
        }
    }

    Type type() const { return m_type; }
    DensityFunction* input() const { return m_input; }

private:
    Type m_type;
    DensityFunction* m_input;
    double m_minValue;
    double m_maxValue;
};

// ============================================================================
// TWO-ARGUMENT FUNCTIONS (ADD, MUL, MIN, MAX)
// ============================================================================

/**
 * MulOrAdd - Optimized implementation when one argument is constant
 * Java: DensityFunctions.MulOrAdd (line 994)
 */
class MulOrAdd : public SimpleFunction {
public:
    enum class SpecificType {
        MUL,
        ADD
    };

    MulOrAdd(SpecificType specificType, DensityFunction* input, double minValue, double maxValue, double argument)
        : m_specificType(specificType), m_input(input), m_minValue(minValue), m_maxValue(maxValue), m_argument(argument) {}

    double compute(const FunctionContext& context) const override {
        double value = m_input->compute(context);
        return transform(value);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        m_input->fillArray(output, count, contextProvider);
        for (int32_t i = 0; i < count; ++i) {
            output[i] = transform(output[i]);
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java lines 1018-1036
        DensityFunction* function = m_input->mapAll(visitor);
        double min = function->minValue();
        double max = function->maxValue();
        double minValue, maxValue;

        if (m_specificType == SpecificType::ADD) {
            minValue = min + m_argument;
            maxValue = max + m_argument;
        } else if (m_argument >= 0.0) {
            minValue = min * m_argument;
            maxValue = max * m_argument;
        } else {
            minValue = max * m_argument;
            maxValue = min * m_argument;
        }

        return new MulOrAdd(m_specificType, function, minValue, maxValue, m_argument);
    }

    double minValue() const override { return m_minValue; }
    double maxValue() const override { return m_maxValue; }

    double transform(double input) const {
        // Java lines 1007-1016
        switch (m_specificType) {
            case SpecificType::MUL:
                return input * m_argument;
            case SpecificType::ADD:
                return input + m_argument;
            default:
                throw std::runtime_error("Unknown MulOrAdd type");
        }
    }

    TwoArgType type() const {
        return m_specificType == SpecificType::MUL ? TwoArgType::MUL : TwoArgType::ADD;
    }

    DensityFunction* argument1() const {
        return new Constant(m_argument);  // Java line 1000
    }

    DensityFunction* argument2() const {
        return m_input;  // Java line 1004
    }

private:
    SpecificType m_specificType;
    DensityFunction* m_input;
    double m_minValue;
    double m_maxValue;
    double m_argument;
};

/**
 * Ap2 - General two-argument function implementation
 * Java: DensityFunctions.Ap2 (line 1049)
 */
class Ap2 : public DensityFunction {
public:
    Ap2(TwoArgType type, DensityFunction* argument1, DensityFunction* argument2, double minValue, double maxValue)
        : m_type(type), m_argument1(argument1), m_argument2(argument2), m_minValue(minValue), m_maxValue(maxValue) {}

    double compute(const FunctionContext& context) const override {
        // Java lines 1050-1062
        double v1 = m_argument1->compute(context);

        switch (m_type) {
            case TwoArgType::ADD:  // case 0
                return v1 + m_argument2->compute(context);

            case TwoArgType::MUL:  // case 1
                // Optimization: if v1 is 0, don't compute v2
                return (v1 == 0.0) ? 0.0 : v1 * m_argument2->compute(context);

            case TwoArgType::MIN:  // case 2
                // Optimization: if v1 < arg2.minValue(), return v1 directly
                return (v1 < m_argument2->minValue()) ? v1 : std::min(v1, m_argument2->compute(context));

            case TwoArgType::MAX:  // case 3
                // Optimization: if v1 > arg2.maxValue(), return v1 directly
                return (v1 > m_argument2->maxValue()) ? v1 : std::max(v1, m_argument2->compute(context));

            default:
                throw std::runtime_error("Unknown TwoArgType");
        }
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java lines 1064-1098
        m_argument1->fillArray(output, count, contextProvider);

        switch (m_type) {
            case TwoArgType::ADD: {  // case 0
                double* v2 = new double[count];
                m_argument2->fillArray(v2, count, contextProvider);
                for (int32_t i = 0; i < count; ++i) {
                    output[i] += v2[i];
                }
                delete[] v2;
                break;
            }

            case TwoArgType::MUL: {  // case 1
                for (int32_t i = 0; i < count; ++i) {
                    double v = output[i];
                    output[i] = (v == 0.0) ? 0.0 : v * m_argument2->compute(*contextProvider.forIndex(i));
                }
                break;
            }

            case TwoArgType::MIN: {  // case 2
                double min = m_argument2->minValue();
                for (int32_t i = 0; i < count; ++i) {
                    double v = output[i];
                    output[i] = (v < min) ? v : std::min(v, m_argument2->compute(*contextProvider.forIndex(i)));
                }
                break;
            }

            case TwoArgType::MAX: {  // case 3
                double max = m_argument2->maxValue();
                for (int32_t i = 0; i < count; ++i) {
                    double v = output[i];
                    output[i] = (v > max) ? v : std::max(v, m_argument2->compute(*contextProvider.forIndex(i)));
                }
                break;
            }
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java lines 1100-1102: create(this.type, this.argument1.mapAll(visitor), this.argument2.mapAll(visitor))
        return createTwoArgumentFunction(m_type, m_argument1->mapAll(visitor), m_argument2->mapAll(visitor));
    }

    double minValue() const override { return m_minValue; }
    double maxValue() const override { return m_maxValue; }

    TwoArgType type() const { return m_type; }
    DensityFunction* argument1() const { return m_argument1; }
    DensityFunction* argument2() const { return m_argument2; }

private:
    TwoArgType m_type;
    DensityFunction* m_argument1;
    DensityFunction* m_argument2;
    double m_minValue;
    double m_maxValue;
};

// ============================================================================
// Y CLAMPED GRADIENT
// ============================================================================

/**
 * YClampedGradient - Linear gradient between two Y levels
 * Java: DensityFunctions.YClampedGradient (line 1222)
 *
 * Maps Y coordinate from [fromY, toY] to [fromValue, toValue] using linear interpolation.
 * Used for terrain height calculations and vertical density gradients.
 */
class YClampedGradient : public SimpleFunction {
public:
    YClampedGradient(int32_t fromY, int32_t toY, double fromValue, double toValue)
        : m_fromY(fromY), m_toY(toY), m_fromValue(fromValue), m_toValue(toValue) {}

    double compute(const FunctionContext& context) const override {
        // Java line 1227: Mth.clampedMap((double)context.blockY(), (double)this.fromY, (double)this.toY, this.fromValue, this.toValue)
        return Mth::clampedMap(
            static_cast<double>(context.blockY()),
            static_cast<double>(m_fromY),
            static_cast<double>(m_toY),
            m_fromValue,
            m_toValue
        );
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Default implementation - delegates to compute()
        contextProvider.fillAllDirectly(output, count, const_cast<YClampedGradient*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // YClampedGradient has no sub-functions, so just return copy
        return visitor.apply(new YClampedGradient(m_fromY, m_toY, m_fromValue, m_toValue));
    }

    double minValue() const override {
        // Java line 1230-1232: Math.min(this.fromValue, this.toValue)
        return std::min(m_fromValue, m_toValue);
    }

    double maxValue() const override {
        // Java line 1234-1236: Math.max(this.fromValue, this.toValue)
        return std::max(m_fromValue, m_toValue);
    }

    // Accessors
    int32_t fromY() const { return m_fromY; }
    int32_t toY() const { return m_toY; }
    double fromValue() const { return m_fromValue; }
    double toValue() const { return m_toValue; }

private:
    int32_t m_fromY;
    int32_t m_toY;
    double m_fromValue;
    double m_toValue;
};

// ============================================================================
// SHIFT NOISE (ShiftA, ShiftB, Shift)
// ============================================================================

/**
 * ShiftA - Shift noise in XZ plane only
 * Java: DensityFunctions.ShiftA (line 721)
 *
 * Samples noise with coordinates (blockX, 0, blockZ) scaled by 0.25, output scaled by 4.0.
 * Used for shifting terrain features horizontally.
 */
class ShiftA : public DensityFunction {
public:
    explicit ShiftA(DensityFunction::NoiseHolder* offsetNoise)
        : m_offsetNoise(offsetNoise) {}

    double compute(const FunctionContext& context) const override {
        // Java line 724-725: compute((double)context.blockX(), (double)0.0F, (double)context.blockZ())
        return computeShift(
            static_cast<double>(context.blockX()),
            static_cast<double>(0.0F),
            static_cast<double>(context.blockZ())
        );
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<ShiftA*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 728-729: visitor.apply(new ShiftA(visitor.visitNoise(this.offsetNoise)))
        return visitor.apply(new ShiftA(visitor.visitNoise(m_offsetNoise)));
    }

    double minValue() const override {
        // Java line 704-706: return -this.maxValue()
        return -maxValue();
    }

    double maxValue() const override {
        // Java line 708-710: return this.offsetNoise().maxValue() * (double)4.0F
        return m_offsetNoise->maxValue() * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* offsetNoise() const { return m_offsetNoise; }

private:
    // Java line 712-714: base compute method
    double computeShift(double localX, double localY, double localZ) const {
        return m_offsetNoise->getValue(
            localX * static_cast<double>(0.25F),
            localY * static_cast<double>(0.25F),
            localZ * static_cast<double>(0.25F)
        ) * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* m_offsetNoise;
};

/**
 * ShiftB - Shift noise in ZX plane (swapped coordinates)
 * Java: DensityFunctions.ShiftB (line 741)
 *
 * Samples noise with coordinates (blockZ, blockX, 0) scaled by 0.25, output scaled by 4.0.
 * Used for independent horizontal shifting.
 */
class ShiftB : public DensityFunction {
public:
    explicit ShiftB(DensityFunction::NoiseHolder* offsetNoise)
        : m_offsetNoise(offsetNoise) {}

    double compute(const FunctionContext& context) const override {
        // Java line 744-745: compute((double)context.blockZ(), (double)context.blockX(), (double)0.0F)
        return computeShift(
            static_cast<double>(context.blockZ()),
            static_cast<double>(context.blockX()),
            static_cast<double>(0.0F)
        );
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<ShiftB*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 748-749: visitor.apply(new ShiftB(visitor.visitNoise(this.offsetNoise)))
        return visitor.apply(new ShiftB(visitor.visitNoise(m_offsetNoise)));
    }

    double minValue() const override {
        return -maxValue();
    }

    double maxValue() const override {
        return m_offsetNoise->maxValue() * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* offsetNoise() const { return m_offsetNoise; }

private:
    double computeShift(double localX, double localY, double localZ) const {
        return m_offsetNoise->getValue(
            localX * static_cast<double>(0.25F),
            localY * static_cast<double>(0.25F),
            localZ * static_cast<double>(0.25F)
        ) * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* m_offsetNoise;
};

/**
 * Shift - Shift noise in all three dimensions
 * Java: DensityFunctions.Shift (line 761)
 *
 * Samples noise with coordinates (blockX, blockY, blockZ) scaled by 0.25, output scaled by 4.0.
 * Used for full 3D terrain shifting.
 */
class Shift : public DensityFunction {
public:
    explicit Shift(DensityFunction::NoiseHolder* offsetNoise)
        : m_offsetNoise(offsetNoise) {}

    double compute(const FunctionContext& context) const override {
        // Java line 764-765: compute((double)context.blockX(), (double)context.blockY(), (double)context.blockZ())
        return computeShift(
            static_cast<double>(context.blockX()),
            static_cast<double>(context.blockY()),
            static_cast<double>(context.blockZ())
        );
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<Shift*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 768-769: visitor.apply(new Shift(visitor.visitNoise(this.offsetNoise)))
        return visitor.apply(new Shift(visitor.visitNoise(m_offsetNoise)));
    }

    double minValue() const override {
        return -maxValue();
    }

    double maxValue() const override {
        return m_offsetNoise->maxValue() * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* offsetNoise() const { return m_offsetNoise; }

private:
    double computeShift(double localX, double localY, double localZ) const {
        return m_offsetNoise->getValue(
            localX * static_cast<double>(0.25F),
            localY * static_cast<double>(0.25F),
            localZ * static_cast<double>(0.25F)
        ) * static_cast<double>(4.0F);
    }

    DensityFunction::NoiseHolder* m_offsetNoise;
};

/**
 * MarkerOrMarked - Interface for density functions that carry optimization markers
 *
 * Reference: DensityFunctions.java lines 411-423
 *
 * Markers are metadata hints that tell the terrain generation system how to
 * optimize evaluation of density functions. The markers themselves don't perform
 * caching - they're just tags that other parts of the system (like NoiseChunk)
 * recognize and use to implement the actual caching/interpolation.
 *
 * Marker types:
 * - Interpolated: Hints that this function should be interpolated
 * - FlatCache: Cache per XZ column (same for all Y at given X,Z)
 * - Cache2D: Cache based on 2D position (ignores Y)
 * - CacheOnce: Simple memoization (cache single value)
 * - CacheAllInCell: Cache all values within a noise cell
 */
class MarkerOrMarked : public DensityFunction {
public:
    enum class Type {
        Interpolated,
        FlatCache,
        Cache2D,
        CacheOnce,
        CacheAllInCell
    };

    virtual Type type() const = 0;
    virtual DensityFunction* wrapped() const = 0;

    // Fast type checking - always WrapType::Marker for MarkerOrMarked
    WrapType getWrapType() const override { return WrapType::Marker; }

    // Convert MarkerOrMarked::Type to DensityFunction::MarkerType
    MarkerType getMarkerType() const override {
        switch (type()) {
            case Type::Interpolated: return MarkerType::Interpolated;
            case Type::FlatCache: return MarkerType::FlatCache;
            case Type::Cache2D: return MarkerType::Cache2D;
            case Type::CacheOnce: return MarkerType::CacheOnce;
            case Type::CacheAllInCell: return MarkerType::CacheAllInCell;
            default: return MarkerType::Interpolated;
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 420-422
        return visitor.apply(createMarker(type(), wrapped()->mapAll(visitor)));
    }

private:
    // Forward declaration - will be defined after Marker class
    static DensityFunction* createMarker(Type type, DensityFunction* wrapped);
};

/**
 * Marker - Transparent wrapper that tags a density function with optimization hints
 *
 * Reference: DensityFunctions.java lines 425-465
 *
 * The Marker class is a simple pass-through wrapper. It delegates all computation
 * to the wrapped function but carries a Type tag that the terrain generation system
 * uses to apply optimizations.
 *
 * Examples:
 * - flatCache(noise(...)) → Tells NoiseChunk to cache this per vertical column
 * - interpolated(continentalness) → Tells NoiseChunk to interpolate this function
 * - cacheOnce(expensive_calculation) → Compute once and reuse the value
 */
class Marker : public MarkerOrMarked {
public:
    Marker(Type markerType, DensityFunction* wrappedFunc)
        : m_type(markerType), m_wrapped(wrappedFunc) {}

    double compute(const FunctionContext& context) const override {
        // Java line 426-428: just delegate to wrapped
        return m_wrapped->compute(context);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 430-432: just delegate to wrapped
        m_wrapped->fillArray(output, count, contextProvider);
    }

    double minValue() const override {
        // Java line 434-436
        return m_wrapped->minValue();
    }

    double maxValue() const override {
        // Java line 438-440
        return m_wrapped->maxValue();
    }

    Type type() const override {
        return m_type;
    }

    DensityFunction* wrapped() const override {
        return m_wrapped;
    }

private:
    Type m_type;
    DensityFunction* m_wrapped;
};

// Implementation of MarkerOrMarked::createMarker (needed after Marker class is defined)
inline DensityFunction* MarkerOrMarked::createMarker(Type type, DensityFunction* wrapped) {
    return new Marker(type, wrapped);
}

// ============================================================================
// SPLINE WRAPPER CLASSES
// ============================================================================

// Import spline classes from util namespace
using util::BoundedFloatFunction;
using util::CubicSpline;

/**
 * Spline - DensityFunction wrapper for CubicSpline
 *
 * Reference: DensityFunctions.java lines 1105-1190
 *
 * This wraps a CubicSpline as a DensityFunction, allowing splines to be used
 * in the density function graph for terrain generation.
 *
 * The spline system is used extensively for terrain shaping:
 * - Continentalness splines
 * - Erosion offset splines
 * - Jaggedness splines
 * - Factor splines
 */
class Spline : public DensityFunction {
public:
    /**
     * Point - Simple wrapper around FunctionContext for spline evaluation
     *
     * Reference: DensityFunctions.Spline.Point (Java line 1188-1189)
     *
     * Used as the context type C for CubicSpline<Point, Coordinate>
     */
    class Point {
    public:
        explicit Point(const FunctionContext& ctx) : m_context(ctx) {}

        const FunctionContext& context() const { return m_context; }

    private:
        const FunctionContext& m_context;
    };

    /**
     * Coordinate - Wraps a DensityFunction as a spline coordinate
     *
     * Reference: DensityFunctions.Spline.Coordinate (Java lines 1140-1186)
     *
     * This allows density functions to be used as coordinates in spline evaluation.
     * For example, erosion or continentalness can be spline coordinates.
     */
    class Coordinate : public BoundedFloatFunction<Point> {
    public:
        explicit Coordinate(DensityFunction* function) : m_function(function) {}

        float apply(const Point& point) const override {
            // Java line 1167-1169: return (float)function.compute(point.context())
            return static_cast<float>(m_function->compute(point.context()));
        }

        float minValue() const override {
            // Java line 1171-1173
            // In Java, checks if Holder.isBound() - we'll assume always bound for now
            return static_cast<float>(m_function->minValue());
        }

        float maxValue() const override {
            // Java line 1175-1177
            return static_cast<float>(m_function->maxValue());
        }

        Coordinate* mapAll(DensityFunction::Visitor& visitor) {
            // Java line 1179-1181
            return new Coordinate(m_function->mapAll(visitor));
        }

        DensityFunction* function() const { return m_function; }

    private:
        DensityFunction* m_function;
    };

    // Type alias for the spline type we use
    using SplineType = CubicSpline<Point, Coordinate>;

    explicit Spline(SplineType* splinePtr) : m_spline(splinePtr) {}

    double compute(const FunctionContext& context) const override {
        // Java line 1110-1112: return (double)this.spline.apply(new Point(context))
        Point point(context);
        return static_cast<double>(m_spline->apply(point));
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 1122-1124
        contextProvider.fillAllDirectly(output, count, const_cast<Spline*>(this));
    }

    double minValue() const override {
        // Java line 1114-1116
        return static_cast<double>(m_spline->minValue());
    }

    double maxValue() const override {
        // Java line 1118-1120
        return static_cast<double>(m_spline->maxValue());
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 1126-1128
        // return visitor.apply(new Spline(this.spline.mapAll((c) -> c.mapAll(visitor))))

        // Create a CoordinateVisitor that wraps each Coordinate by calling mapAll(visitor)
        class SplineCoordinateVisitor : public SplineType::CoordinateVisitor {
        private:
            Visitor& m_dfVisitor;
        public:
            explicit SplineCoordinateVisitor(Visitor& v) : m_dfVisitor(v) {}
            Coordinate* visit(Coordinate* c) override {
                return c->mapAll(m_dfVisitor);
            }
        };

        SplineCoordinateVisitor coordVisitor(visitor);
        SplineType* mappedSpline = m_spline->mapAll(coordVisitor);
        return visitor.apply(new Spline(mappedSpline));
    }

    SplineType* spline() const { return m_spline; }

private:
    SplineType* m_spline;
};

/**
 * RangeChoice - Conditional density function that selects between two functions
 * based on whether an input value is within a specified range.
 *
 * Reference: DensityFunctions.java lines 657-699
 *
 * Logic:
 * - Evaluates the input density function
 * - If value is in [minInclusive, maxExclusive), returns whenInRange.compute()
 * - Otherwise, returns whenOutOfRange.compute()
 *
 * Example usage in Minecraft:
 * - Switching between different terrain generation based on height
 * - Conditional blending of noise functions
 */
class RangeChoice : public DensityFunction {
public:
    RangeChoice(DensityFunction* input, double minInclusive, double maxExclusive,
                DensityFunction* whenInRange, DensityFunction* whenOutOfRange)
        : m_input(input)
        , m_minInclusive(minInclusive)
        , m_maxExclusive(maxExclusive)
        , m_whenInRange(whenInRange)
        , m_whenOutOfRange(whenOutOfRange) {}

    double compute(const FunctionContext& context) const override {
        // Java line 661-664
        double inputValue = m_input->compute(context);
        return (inputValue >= m_minInclusive && inputValue < m_maxExclusive)
            ? m_whenInRange->compute(context)
            : m_whenOutOfRange->compute(context);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 666-678
        // First fill array with input values
        m_input->fillArray(output, count, contextProvider);

        // Then replace each value based on range check
        for (int32_t i = 0; i < count; ++i) {
            double v = output[i];
            if (v >= m_minInclusive && v < m_maxExclusive) {
                output[i] = m_whenInRange->compute(*contextProvider.forIndex(i));
            } else {
                output[i] = m_whenOutOfRange->compute(*contextProvider.forIndex(i));
            }
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 680-682
        return visitor.apply(new RangeChoice(
            m_input->mapAll(visitor),
            m_minInclusive,
            m_maxExclusive,
            m_whenInRange->mapAll(visitor),
            m_whenOutOfRange->mapAll(visitor)
        ));
    }

    double minValue() const override {
        // Java line 684-686
        return std::min(m_whenInRange->minValue(), m_whenOutOfRange->minValue());
    }

    double maxValue() const override {
        // Java line 688-690
        return std::max(m_whenInRange->maxValue(), m_whenOutOfRange->maxValue());
    }

    // Accessors for members
    DensityFunction* input() const { return m_input; }
    double minInclusive() const { return m_minInclusive; }
    double maxExclusive() const { return m_maxExclusive; }
    DensityFunction* whenInRange() const { return m_whenInRange; }
    DensityFunction* whenOutOfRange() const { return m_whenOutOfRange; }

private:
    DensityFunction* m_input;
    double m_minInclusive;
    double m_maxExclusive;
    DensityFunction* m_whenInRange;
    DensityFunction* m_whenOutOfRange;
};

// ============================================================================
// FACTORY FUNCTIONS
// ============================================================================

/**
 * Factory functions matching Java API
 */
inline DensityFunction* constant(double value) {
    return new Constant(value);
}

inline DensityFunction* zero() {
    return Constant::ZERO();
}

inline DensityFunction* noise(DensityFunction::NoiseHolder* noiseData, double xzScale, double yScale) {
    return new Noise(noiseData, xzScale, yScale);
}

inline DensityFunction* clamp(DensityFunction* input, double min, double max) {
    return new Clamp(input, min, max);
}

inline DensityFunction* add(DensityFunction* f1, DensityFunction* f2) {
    return createTwoArgumentFunction(TwoArgType::ADD, f1, f2);
}

inline DensityFunction* mul(DensityFunction* f1, DensityFunction* f2) {
    return createTwoArgumentFunction(TwoArgType::MUL, f1, f2);
}

inline DensityFunction* min(DensityFunction* f1, DensityFunction* f2) {
    return createTwoArgumentFunction(TwoArgType::MIN, f1, f2);
}

inline DensityFunction* max(DensityFunction* f1, DensityFunction* f2) {
    return createTwoArgumentFunction(TwoArgType::MAX, f1, f2);
}

// Mapped transformation helpers (matching Java API)
inline DensityFunction* abs(DensityFunction* func) {
    return Mapped::create(Mapped::Type::ABS, func);
}

inline DensityFunction* square(DensityFunction* func) {
    return Mapped::create(Mapped::Type::SQUARE, func);
}

inline DensityFunction* cube(DensityFunction* func) {
    return Mapped::create(Mapped::Type::CUBE, func);
}

inline DensityFunction* halfNegative(DensityFunction* func) {
    return Mapped::create(Mapped::Type::HALF_NEGATIVE, func);
}

inline DensityFunction* quarterNegative(DensityFunction* func) {
    return Mapped::create(Mapped::Type::QUARTER_NEGATIVE, func);
}

inline DensityFunction* invert(DensityFunction* func) {
    return Mapped::create(Mapped::Type::INVERT, func);
}

inline DensityFunction* squeeze(DensityFunction* func) {
    return Mapped::create(Mapped::Type::SQUEEZE, func);
}

// YClampedGradient factory (Java line 200)
inline DensityFunction* yClampedGradient(int32_t fromY, int32_t toY, double fromValue, double toValue) {
    return new YClampedGradient(fromY, toY, fromValue, toValue);
}

// RangeChoice factory (Java line 143)
inline DensityFunction* rangeChoice(DensityFunction* input, double minInclusive, double maxExclusive,
                                     DensityFunction* whenInRange, DensityFunction* whenOutOfRange) {
    return new RangeChoice(input, minInclusive, maxExclusive, whenInRange, whenOutOfRange);
}

// Shift noise factories
inline DensityFunction* shiftA(DensityFunction::NoiseHolder* offsetNoise) {
    return new ShiftA(offsetNoise);
}

inline DensityFunction* shiftB(DensityFunction::NoiseHolder* offsetNoise) {
    return new ShiftB(offsetNoise);
}

inline DensityFunction* shift(DensityFunction::NoiseHolder* offsetNoise) {
    return new Shift(offsetNoise);
}

// Marker factories (Java lines 95-113)
// These create optimization hints for the terrain generation system
inline DensityFunction* interpolated(DensityFunction* function) {
    return new Marker(MarkerOrMarked::Type::Interpolated, function);
}

inline DensityFunction* flatCache(DensityFunction* function) {
    return new Marker(MarkerOrMarked::Type::FlatCache, function);
}

inline DensityFunction* cache2d(DensityFunction* function) {
    return new Marker(MarkerOrMarked::Type::Cache2D, function);
}

inline DensityFunction* cacheOnce(DensityFunction* function) {
    return new Marker(MarkerOrMarked::Type::CacheOnce, function);
}

inline DensityFunction* cacheAllInCell(DensityFunction* function) {
    return new Marker(MarkerOrMarked::Type::CacheAllInCell, function);
}

// Spline factory
// Creates a DensityFunction that wraps a CubicSpline
inline DensityFunction* spline(Spline::SplineType* splinePtr) {
    return new Spline(splinePtr);
}

// Lerp (Linear Interpolation) factories
// Java lines 221-233

// Forward declaration for optimized version
inline DensityFunction* lerp(DensityFunction* factor, double first, DensityFunction* second);

/**
 * lerp - Linear interpolation between two density functions
 *
 * Formula: first * (1 - alpha) + second * alpha
 *        = first + alpha * (second - first)
 *
 * @param alpha Interpolation factor (0.0 = first, 1.0 = second)
 * @param first First value
 * @param second Second value
 * @return Interpolated density function
 *
 * Java: if first is Constant, uses optimized version lerp(alpha, constant, second)
 * Otherwise: add(mul(first, add(mul(cacheOnce(alpha), constant(-1.0)), constant(1.0))), mul(second, cacheOnce(alpha)))
 */
inline DensityFunction* lerp(DensityFunction* alpha, DensityFunction* first, DensityFunction* second) {
    // Check if first is a constant for optimization
    if (Constant* constFirst = dynamic_cast<Constant*>(first)) {
        // Use optimized version: first + alpha * (second - first)
        return lerp(alpha, constFirst->value(), second);
    } else {
        // General case: first * (1 - alpha) + second * alpha
        // alphaCached = cacheOnce(alpha)
        DensityFunction* alphaCached = cacheOnce(alpha);

        // oneMinusAlpha = alpha * -1.0 + 1.0 = 1.0 - alpha
        DensityFunction* oneMinusAlpha = add(
            mul(alphaCached, constant(-1.0)),
            constant(1.0)
        );

        // result = first * oneMinusAlpha + second * alphaCached
        return add(
            mul(first, oneMinusAlpha),
            mul(second, alphaCached)
        );
    }
}

/**
 * lerp - Linear interpolation with constant first value (optimized)
 *
 * Formula: first + factor * (second - first)
 *
 * @param factor Interpolation factor
 * @param first First value (constant)
 * @param second Second value
 * @return Interpolated density function
 *
 * Java: add(mul(factor, add(second, constant(-first))), constant(first))
 */
inline DensityFunction* lerp(DensityFunction* factor, double first, DensityFunction* second) {
    // first + factor * (second - first)
    // = factor * (second - first) + first
    return add(
        mul(factor, add(second, constant(-first))),
        constant(first)
    );
}

// ShiftedNoise factory
// Java line 127-129

/**
 * shiftedNoise2d - 2D shifted noise (Y shift = 0, yScale = 0)
 *
 * This is used for climate parameters that vary only in XZ plane.
 *
 * @param shiftX X-axis shift function
 * @param shiftZ Z-axis shift function
 * @param xzScale Horizontal scale
 * @param noiseData Noise parameters
 * @return Shifted noise density function
 *
 * Java: new ShiftedNoise(shiftX, zero(), shiftZ, xzScale, 0.0, new NoiseHolder(noiseData))
 */
inline DensityFunction* shiftedNoise2d(DensityFunction* shiftX, DensityFunction* shiftZ,
                                       double xzScale, DensityFunction::NoiseHolder* noiseData) {
    // Y shift is zero, yScale is 0.0
    return new ShiftedNoise(shiftX, zero(), shiftZ, xzScale, 0.0, noiseData);
}

// Blender factory functions
// Java lines 159-161, 213-215, 217-219

/**
 * blendAlpha - Returns singleton that always produces 1.0
 * Java: DensityFunctions.blendAlpha() returns BlendAlpha.INSTANCE
 */
inline DensityFunction* blendAlpha() {
    return BlendAlpha::instance();
}

/**
 * blendOffset - Returns singleton that always produces 0.0
 * Java: DensityFunctions.blendOffset() returns BlendOffset.INSTANCE
 */
inline DensityFunction* blendOffset() {
    return BlendOffset::instance();
}

/**
 * blendDensity - Applies terrain blending to input density
 * Java: DensityFunctions.blendDensity(input) returns new BlendDensity(input)
 */
inline DensityFunction* blendDensity(DensityFunction* input) {
    return new BlendDensity(input);
}

// ============================================================================
// WEIRD SCALED SAMPLER (for cave generation)
// ============================================================================

/**
 * WeirdScaledSampler - Samples noise at positions scaled by rarity factor
 * Java: DensityFunctions.WeirdScaledSampler (line 566-619)
 *
 * This is used for spaghetti caves where the sampling frequency depends on
 * a rarity modifier. Higher rarity = larger scale = rarer caves.
 */
class WeirdScaledSampler : public DensityFunction {
public:
    enum class RarityValueMapper {
        TYPE1,  // For 3D spaghetti caves
        TYPE2   // For 2D spaghetti caves
    };

    WeirdScaledSampler(DensityFunction* input, DensityFunction::NoiseHolder* noise, RarityValueMapper mapper)
        : m_input(input), m_noise(noise), m_mapper(mapper) {}

    // Java line 570-573
    double compute(const FunctionContext& context) const override {
        double inputValue = m_input->compute(context);
        double rarity = getRarityValue(inputValue);
        // Sample noise at position scaled by rarity
        return rarity * std::abs(m_noise->getValue(
            static_cast<double>(context.blockX()) / rarity,
            static_cast<double>(context.blockY()) / rarity,
            static_cast<double>(context.blockZ()) / rarity
        ));
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<WeirdScaledSampler*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(new WeirdScaledSampler(
            m_input->mapAll(visitor),
            visitor.visitNoise(m_noise),
            m_mapper
        ));
    }

    // Java line 579-580
    double minValue() const override {
        return 0.0;
    }

    // Java line 583-585
    double maxValue() const override {
        return getMaxRarity() * m_noise->maxValue();
    }

    DensityFunction* input() const { return m_input; }
    DensityFunction::NoiseHolder* noise() const { return m_noise; }
    RarityValueMapper mapper() const { return m_mapper; }

private:
    DensityFunction* m_input;
    DensityFunction::NoiseHolder* m_noise;
    RarityValueMapper m_mapper;

    // Java: NoiseRouterData.QuantizedSpaghettiRarity (line 337-345)
    // Reference: NoiseRouterData.java line 596-597
    double getRarityValue(double inputValue) const {
        if (m_mapper == RarityValueMapper::TYPE1) {
            // getSpaghettiRarity3D
            if (inputValue < -0.5) {
                return 0.75;
            } else if (inputValue < 0.0) {
                return 1.0;
            } else {
                return inputValue < 0.5 ? 1.5 : 2.0;
            }
        } else {
            // getSphaghettiRarity2D (TYPE2)
            if (inputValue < -0.75) {
                return 0.5;
            } else if (inputValue < -0.5) {
                return 0.75;
            } else if (inputValue < 0.5) {
                return 1.0;
            } else {
                return inputValue < 0.75 ? 2.0 : 3.0;
            }
        }
    }

    double getMaxRarity() const {
        return m_mapper == RarityValueMapper::TYPE1 ? 2.0 : 3.0;
    }
};

// ============================================================================
// HELPER FUNCTIONS FOR CAVE GENERATION
// ============================================================================

/**
 * mapFromUnitTo - Maps a function from [-1, 1] range to [min, max] range
 * Java: DensityFunctions.mapFromUnitTo() (private method, line 207-211)
 *
 * Formula: middle + factor * function
 * where middle = (min + max) / 2, factor = (max - min) / 2
 */
inline DensityFunction* mapFromUnitTo(DensityFunction* function, double min, double max) {
    double middle = (min + max) * 0.5;
    double factor = (max - min) * 0.5;
    return add(constant(middle), mul(constant(factor), function));
}

/**
 * mappedNoise - Creates noise mapped to specific range
 * Java: DensityFunctions.mappedNoise() (line 115-124)
 */
inline DensityFunction* mappedNoise(DensityFunction::NoiseHolder* noiseData, double xzScale, double yScale, double minTarget, double maxTarget) {
    return mapFromUnitTo(noise(noiseData, xzScale, yScale), minTarget, maxTarget);
}

inline DensityFunction* mappedNoise(DensityFunction::NoiseHolder* noiseData, double yScale, double minTarget, double maxTarget) {
    return mappedNoise(noiseData, 1.0, yScale, minTarget, maxTarget);
}

inline DensityFunction* mappedNoise(DensityFunction::NoiseHolder* noiseData, double minTarget, double maxTarget) {
    return mappedNoise(noiseData, 1.0, 1.0, minTarget, maxTarget);
}

/**
 * weirdScaledSampler - Creates a weird scaled sampler for cave generation
 * Java: DensityFunctions.weirdScaledSampler() (line 167-169)
 */
inline DensityFunction* weirdScaledSampler(DensityFunction* input, DensityFunction::NoiseHolder* noiseData, WeirdScaledSampler::RarityValueMapper mapper) {
    return new WeirdScaledSampler(input, noiseData, mapper);
}

// ============================================================================
// END ISLAND DENSITY FUNCTION
// ============================================================================

} // namespace DensityFunctions - temporarily close for EndIslandDensityFunction

/**
 * EndIslandDensityFunction - Generates the island pattern for The End dimension
 * Java: DensityFunctions.EndIslandDensityFunction (line 512-564)
 *
 * This uses a SimplexNoise seeded with LegacyRandomSource to create the
 * characteristic outer end islands pattern.
 *
 * Note: This class is in the minecraft::density namespace (not DensityFunctions)
 * to properly inherit from SimpleFunction.
 */
class EndIslandDensityFunction : public SimpleFunction {
public:
    static constexpr float ISLAND_THRESHOLD = -0.9F;

    explicit EndIslandDensityFunction(int64_t seed);
    ~EndIslandDensityFunction();

    double compute(const FunctionContext& context) const override;

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Use default SimpleFunction implementation - compute for each point
        for (int32_t i = 0; i < count; ++i) {
            output[i] = compute(*contextProvider.forIndex(i));
        }
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // EndIslandDensityFunction has no sub-functions to map
        // Return a copy wrapped by visitor
        return visitor.apply(new EndIslandDensityFunction(0L));
    }

    double minValue() const override {
        // Java line 553-555: (double)-0.84375F
        return static_cast<double>(-0.84375F);
    }

    double maxValue() const override {
        // Java line 557-559: (double)0.5625F
        return static_cast<double>(0.5625F);
    }

    /**
     * getHeightValue - Calculate the height contribution at a section position
     * Java line 523-547
     */
    static float getHeightValue(::minecraft::synth::SimplexNoise* islandNoise, int sectionX, int sectionZ);

private:
    ::minecraft::synth::SimplexNoise* m_islandNoise;
};

namespace DensityFunctions { // reopen namespace

// ============================================================================
// FIND TOP SURFACE
// ============================================================================

/**
 * FindTopSurface - Binary search to find the top surface Y level
 * Java: DensityFunctions.FindTopSurface (line 1247-1289)
 *
 * This function finds the Y coordinate where density transitions from
 * positive to negative (i.e., where the surface is). It's used for
 * preliminary surface level calculation in aquifer placement.
 */
class FindTopSurface : public DensityFunction {
public:
    FindTopSurface(DensityFunction* density, DensityFunction* upperBound, int lowerBound, int cellHeight)
        : m_density(density)
        , m_upperBound(upperBound)
        , m_lowerBound(lowerBound)
        , m_cellHeight(cellHeight) {}

    double compute(const FunctionContext& context) const override {
        // Java line 1251-1263
        int topY = static_cast<int>(std::floor(m_upperBound->compute(context) / static_cast<double>(m_cellHeight))) * m_cellHeight;

        if (topY <= m_lowerBound) {
            return static_cast<double>(m_lowerBound);
        }

        // Search downward from topY to lowerBound in cellHeight steps
        for (int blockY = topY; blockY >= m_lowerBound; blockY -= m_cellHeight) {
            // Create a single point context at this Y level
            SinglePointContext singlePointContext(context.blockX(), blockY, context.blockZ());
            if (m_density->compute(singlePointContext) > static_cast<double>(0.0F)) {
                return static_cast<double>(blockY);
            }
        }

        return static_cast<double>(m_lowerBound);
    }

    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        // Java line 1266-1268
        contextProvider.fillAllDirectly(output, count, const_cast<FindTopSurface*>(this));
    }

    DensityFunction* mapAll(Visitor& visitor) override {
        // Java line 1270-1272
        return visitor.apply(new FindTopSurface(
            m_density->mapAll(visitor),
            m_upperBound->mapAll(visitor),
            m_lowerBound,
            m_cellHeight
        ));
    }

    double minValue() const override {
        // Java line 1274-1276
        return static_cast<double>(m_lowerBound);
    }

    double maxValue() const override {
        // Java line 1278-1280
        return std::max(static_cast<double>(m_lowerBound), m_upperBound->maxValue());
    }

    // Accessors
    DensityFunction* density() const { return m_density; }
    DensityFunction* upperBound() const { return m_upperBound; }
    int lowerBound() const { return m_lowerBound; }
    int cellHeight() const { return m_cellHeight; }

private:
    DensityFunction* m_density;
    DensityFunction* m_upperBound;
    int m_lowerBound;
    int m_cellHeight;
};

// ============================================================================
// ADDITIONAL FACTORY FUNCTIONS
// ============================================================================

/**
 * endIslands - Creates end island density function
 * Java: DensityFunctions.endIslands() (line 163-165)
 */
inline DensityFunction* endIslands(int64_t seed) {
    return new EndIslandDensityFunction(seed);
}

/**
 * findTopSurface - Creates a find top surface density function
 * Java: DensityFunctions.findTopSurface() (line 235-237)
 */
inline DensityFunction* findTopSurface(DensityFunction* density, DensityFunction* upperBound, int lowerBound, int stepSize) {
    return new FindTopSurface(density, upperBound, lowerBound, stepSize);
}

} // namespace DensityFunctions
} // namespace density
} // namespace minecraft
