#pragma once

#include <vector>
#include <algorithm>
#include <limits>
#include <cmath>
#include <stdexcept>
#include <functional>
#include <memory>

namespace minecraft {
namespace util {

/**
 * BoundedFloatFunction - Interface for functions that map C -> float with known bounds
 *
 * Reference: BoundedFloatFunction.java
 *
 * Used by CubicSpline to represent coordinate transformations.
 */
template<typename C>
class BoundedFloatFunction {
public:
    virtual ~BoundedFloatFunction() = default;

    virtual float apply(const C& c) const = 0;
    virtual float minValue() const = 0;
    virtual float maxValue() const = 0;

    // Static helpers for creating transformers
    static BoundedFloatFunction<float>* createUnlimited(std::function<float(float)> function);
    static BoundedFloatFunction<float>* identity();
};

// Implementation of lambda-based BoundedFloatFunction
class UnlimitedFloatFunction : public BoundedFloatFunction<float> {
private:
    std::function<float(float)> m_function;

public:
    UnlimitedFloatFunction(std::function<float(float)> function)
        : m_function(function) {}

    float apply(const float& c) const override {
        return m_function(c);
    }

    float minValue() const override {
        return -std::numeric_limits<float>::infinity();
    }

    float maxValue() const override {
        return std::numeric_limits<float>::infinity();
    }
};

/**
 * CubicSpline - Cubic spline interpolation system
 *
 * Reference: CubicSpline.java
 *
 * Template parameters:
 * - C: Context type (usually Spline::Point for density functions)
 * - I: Coordinate type (must extend BoundedFloatFunction<C>)
 *
 * This implements smooth cubic interpolation between multiple control points.
 * Used extensively in Minecraft for terrain shaping (continentalness, erosion, etc.)
 */
template<typename C, typename I>
class CubicSpline : public BoundedFloatFunction<C> {
public:
    virtual ~CubicSpline() = default;

    // Core interface
    virtual float apply(const C& c) const override = 0;
    virtual float minValue() const override = 0;
    virtual float maxValue() const override = 0;

    // Debug/testing
    virtual std::string parityString() const = 0;

    // Forward declarations for nested classes
    class Constant;
    class Multipoint;
    class Builder;

    /**
     * CoordinateVisitor - Visitor pattern for transforming coordinates
     */
    class CoordinateVisitor {
    public:
        virtual ~CoordinateVisitor() = default;
        virtual I* visit(I* input) = 0;
    };

    virtual CubicSpline<C, I>* mapAll(CoordinateVisitor& visitor) const = 0;

    // Factory methods
    static CubicSpline<C, I>* constant(float value);
    static Builder builder(I* coordinate);
    static Builder builder(I* coordinate, BoundedFloatFunction<float>* valueTransformer);
};

/**
 * CubicSpline::Constant - Simplest spline that returns a constant value
 *
 * Reference: CubicSpline.Constant (Java lines 195-215)
 */
template<typename C, typename I>
class CubicSpline<C, I>::Constant : public CubicSpline<C, I> {
public:
    explicit Constant(float val) : m_value(val) {}

    float apply(const C& c) const override {
        (void)c;  // Unused
        return m_value;
    }

    float minValue() const override {
        return m_value;
    }

    float maxValue() const override {
        return m_value;
    }

    std::string parityString() const override {
        char buf[32];
        snprintf(buf, sizeof(buf), "k=%.3f", m_value);
        return std::string(buf);
    }

    CubicSpline<C, I>* mapAll(typename CubicSpline<C, I>::CoordinateVisitor& visitor) const override {
        (void)visitor;  // Constant doesn't have coordinates to visit
        return new Constant(m_value);
    }

    float value() const { return m_value; }

private:
    float m_value;
};

/**
 * CubicSpline::Multipoint - Multi-point cubic spline interpolation
 *
 * Reference: CubicSpline.Multipoint (Java lines 72-192)
 *
 * This is the heart of Minecraft's terrain shaping system.
 * It performs smooth cubic interpolation between multiple control points,
 * with specified derivatives at each point.
 */
template<typename C, typename I>
class CubicSpline<C, I>::Multipoint : public CubicSpline<C, I> {
public:
    // Constructor validates inputs
    Multipoint(I* coordinate,
               const std::vector<float>& locations,
               const std::vector<CubicSpline<C, I>*>& values,
               const std::vector<float>& derivatives,
               float minVal,
               float maxVal)
        : m_coordinate(coordinate)
        , m_locations(locations)
        , m_values(values)
        , m_derivatives(derivatives)
        , m_minValue(minVal)
        , m_maxValue(maxVal)
    {
        validateSizes(m_locations, m_values, m_derivatives);
    }

    // Factory method that calculates min/max bounds
    static Multipoint* create(
        I* coordinate,
        const std::vector<float>& locations,
        const std::vector<CubicSpline<C, I>*>& values,
        const std::vector<float>& derivatives);

    float apply(const C& c) const override;

    float minValue() const override {
        return m_minValue;
    }

    float maxValue() const override {
        return m_maxValue;
    }

    std::string parityString() const override;

    CubicSpline<C, I>* mapAll(typename CubicSpline<C, I>::CoordinateVisitor& visitor) const override;

    // Accessors
    I* coordinate() const { return m_coordinate; }
    const std::vector<float>& locations() const { return m_locations; }
    const std::vector<CubicSpline<C, I>*>& values() const { return m_values; }
    const std::vector<float>& derivatives() const { return m_derivatives; }

private:
    static void validateSizes(
        const std::vector<float>& locations,
        const std::vector<CubicSpline<C, I>*>& values,
        const std::vector<float>& derivatives);

    static float linearExtend(
        float input,
        const std::vector<float>& locations,
        float value,
        const std::vector<float>& derivatives,
        int index);

    static int findIntervalStart(const std::vector<float>& locations, float input);

    I* m_coordinate;
    std::vector<float> m_locations;
    std::vector<CubicSpline<C, I>*> m_values;
    std::vector<float> m_derivatives;
    float m_minValue;
    float m_maxValue;
};

/**
 * CubicSpline::Builder - Fluent builder for constructing splines
 *
 * Reference: CubicSpline.Builder (Java lines 217-266)
 *
 * Provides a convenient API for building multi-point splines.
 */
template<typename C, typename I>
class CubicSpline<C, I>::Builder {
public:
    explicit Builder(I* coordinate)
        : m_coordinate(coordinate)
        , m_valueTransformer(nullptr)
    {}

    Builder(I* coordinate, BoundedFloatFunction<float>* valueTransformer)
        : m_coordinate(coordinate)
        , m_valueTransformer(valueTransformer)
    {}

    // Add point with constant value
    Builder& addPoint(float location, float value);

    // Add point with constant value and derivative
    Builder& addPoint(float location, float value, float derivative);

    // Add point with spline value
    Builder& addPoint(float location, CubicSpline<C, I>* sampler);

    // Build the final spline
    CubicSpline<C, I>* build();

private:
    Builder& addPoint(float location, CubicSpline<C, I>* sampler, float derivative);

    I* m_coordinate;
    BoundedFloatFunction<float>* m_valueTransformer;
    std::vector<float> m_locations;
    std::vector<CubicSpline<C, I>*> m_values;
    std::vector<float> m_derivatives;
};

// ============================================================================
// IMPLEMENTATION - Multipoint::create
// ============================================================================

template<typename C, typename I>
typename CubicSpline<C, I>::Multipoint*
CubicSpline<C, I>::Multipoint::create(
    I* coordinate,
    const std::vector<float>& locations,
    const std::vector<CubicSpline<C, I>*>& values,
    const std::vector<float>& derivatives)
{
    // Java lines 77-132
    validateSizes(locations, values, derivatives);

    int lastIndex = locations.size() - 1;
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();

    float minInput = coordinate->minValue();
    float maxInput = coordinate->maxValue();

    // Check if input extends below first location
    if (minInput < locations[0]) {
        float edge1 = linearExtend(minInput, locations, values[0]->minValue(), derivatives, 0);
        float edge2 = linearExtend(minInput, locations, values[0]->maxValue(), derivatives, 0);
        minValue = std::min(minValue, std::min(edge1, edge2));
        maxValue = std::max(maxValue, std::max(edge1, edge2));
    }

    // Check if input extends above last location
    if (maxInput > locations[lastIndex]) {
        float edge1 = linearExtend(maxInput, locations, values[lastIndex]->minValue(), derivatives, lastIndex);
        float edge2 = linearExtend(maxInput, locations, values[lastIndex]->maxValue(), derivatives, lastIndex);
        minValue = std::min(minValue, std::min(edge1, edge2));
        maxValue = std::max(maxValue, std::max(edge1, edge2));
    }

    // Accumulate min/max from all value splines
    for (CubicSpline<C, I>* value : values) {
        minValue = std::min(minValue, value->minValue());
        maxValue = std::max(maxValue, value->maxValue());
    }

    // Calculate bounds considering cubic interpolation between points
    // Java lines 103-129
    for (int i = 0; i < lastIndex; ++i) {
        float x1 = locations[i];
        float x2 = locations[i + 1];
        float xDiff = x2 - x1;
        CubicSpline<C, I>* v1 = values[i];
        CubicSpline<C, I>* v2 = values[i + 1];
        float min1 = v1->minValue();
        float max1 = v1->maxValue();
        float min2 = v2->minValue();
        float max2 = v2->maxValue();
        float d1 = derivatives[i];
        float d2 = derivatives[i + 1];

        if (d1 != 0.0f || d2 != 0.0f) {
            float p1 = d1 * xDiff;
            float p2 = d2 * xDiff;
            float minLerp1 = std::min(min1, min2);
            float maxLerp1 = std::max(max1, max2);
            float minA = p1 - max2 + min1;
            float maxA = p1 - min2 + max1;
            float minB = -p2 + min2 - max1;
            float maxB = -p2 + max2 - min1;
            float minLerp2 = std::min(minA, minB);
            float maxLerp2 = std::max(maxA, maxB);
            minValue = std::min(minValue, minLerp1 + 0.25f * minLerp2);
            maxValue = std::max(maxValue, maxLerp1 + 0.25f * maxLerp2);
        }
    }

    return new Multipoint(coordinate, locations, values, derivatives, minValue, maxValue);
}

// ============================================================================
// IMPLEMENTATION - Multipoint::apply
// ============================================================================

template<typename C, typename I>
float CubicSpline<C, I>::Multipoint::apply(const C& c) const {
    // Java lines 149-172
    float input = m_coordinate->apply(c);
    int start = findIntervalStart(m_locations, input);
    int lastIndex = m_locations.size() - 1;

    // Before first point - linear extrapolation
    if (start < 0) {
        return linearExtend(input, m_locations, m_values[0]->apply(c), m_derivatives, 0);
    }

    // After last point - linear extrapolation
    if (start == lastIndex) {
        return linearExtend(input, m_locations, m_values[lastIndex]->apply(c), m_derivatives, lastIndex);
    }

    // Between two points - cubic interpolation
    float x1 = m_locations[start];
    float x2 = m_locations[start + 1];
    float t = (input - x1) / (x2 - x1);  // Normalized position [0, 1]

    CubicSpline<C, I>* f1 = m_values[start];
    CubicSpline<C, I>* f2 = m_values[start + 1];
    float d1 = m_derivatives[start];
    float d2 = m_derivatives[start + 1];

    float y1 = f1->apply(c);
    float y2 = f2->apply(c);

    // Cubic Hermite interpolation
    float a = d1 * (x2 - x1) - (y2 - y1);
    float b = -d2 * (x2 - x1) + (y2 - y1);

    // lerp(t, y1, y2) + t * (1 - t) * lerp(t, a, b)
    float lerpY = y1 + t * (y2 - y1);
    float lerpAB = a + t * (b - a);
    float offset = lerpY + t * (1.0f - t) * lerpAB;

    return offset;
}

// ============================================================================
// IMPLEMENTATION - Helper functions
// ============================================================================

template<typename C, typename I>
void CubicSpline<C, I>::Multipoint::validateSizes(
    const std::vector<float>& locations,
    const std::vector<CubicSpline<C, I>*>& values,
    const std::vector<float>& derivatives)
{
    // Java lines 139-147
    if (locations.size() != values.size() || locations.size() != derivatives.size()) {
        throw std::invalid_argument("All lengths must be equal");
    }
    if (locations.empty()) {
        throw std::invalid_argument("Cannot create a multipoint spline with no points");
    }
}

template<typename C, typename I>
float CubicSpline<C, I>::Multipoint::linearExtend(
    float input,
    const std::vector<float>& locations,
    float value,
    const std::vector<float>& derivatives,
    int index)
{
    // Java lines 134-137
    float derivative = derivatives[index];
    return derivative == 0.0f ? value : value + derivative * (input - locations[index]);
}

template<typename C, typename I>
int CubicSpline<C, I>::Multipoint::findIntervalStart(
    const std::vector<float>& locations,
    float input)
{
    // Java lines 174-176
    // Binary search to find the interval containing input
    // Returns -1 if input < locations[0]
    // Returns size-1 if input >= locations[size-1]

    int from = 0;
    int to = locations.size();

    // Mth.binarySearch equivalent
    int len = to - from;
    while (len > 0) {
        int half = len / 2;
        int middle = from + half;

        if (input < locations[middle]) {
            len = half;
        } else {
            from = middle + 1;
            len = len - half - 1;
        }
    }

    return from - 1;
}

template<typename C, typename I>
std::string CubicSpline<C, I>::Multipoint::parityString() const {
    // Java lines 179-182 - simplified for C++
    return "Multipoint{...}";  // Full implementation would format all points
}

template<typename C, typename I>
CubicSpline<C, I>* CubicSpline<C, I>::Multipoint::mapAll(
    typename CubicSpline<C, I>::CoordinateVisitor& visitor) const
{
    // Java line 189-191
    std::vector<CubicSpline<C, I>*> mappedValues;
    for (CubicSpline<C, I>* v : m_values) {
        mappedValues.push_back(v->mapAll(visitor));
    }

    return Multipoint::create(
        visitor.visit(m_coordinate),
        m_locations,
        mappedValues,
        m_derivatives
    );
}

// ============================================================================
// IMPLEMENTATION - Builder
// ============================================================================

template<typename C, typename I>
typename CubicSpline<C, I>::Builder& CubicSpline<C, I>::Builder::addPoint(
    float location, float value)
{
    // Java lines 236-238
    float transformedValue = m_valueTransformer ? m_valueTransformer->apply(value) : value;
    return addPoint(location, new Constant(transformedValue), 0.0f);
}

template<typename C, typename I>
typename CubicSpline<C, I>::Builder& CubicSpline<C, I>::Builder::addPoint(
    float location, float value, float derivative)
{
    // Java lines 240-242
    float transformedValue = m_valueTransformer ? m_valueTransformer->apply(value) : value;
    return addPoint(location, new Constant(transformedValue), derivative);
}

template<typename C, typename I>
typename CubicSpline<C, I>::Builder& CubicSpline<C, I>::Builder::addPoint(
    float location, CubicSpline<C, I>* sampler)
{
    // Java lines 244-246
    return addPoint(location, sampler, 0.0f);
}

template<typename C, typename I>
typename CubicSpline<C, I>::Builder& CubicSpline<C, I>::Builder::addPoint(
    float location, CubicSpline<C, I>* sampler, float derivative)
{
    // Java lines 248-257
    if (!m_locations.empty() && location <= m_locations.back()) {
        throw std::invalid_argument("Please register points in ascending order");
    }

    m_locations.push_back(location);
    m_values.push_back(sampler);
    m_derivatives.push_back(derivative);

    return *this;
}

template<typename C, typename I>
CubicSpline<C, I>* CubicSpline<C, I>::Builder::build() {
    // Java lines 259-265
    if (m_locations.empty()) {
        throw std::invalid_argument("No elements added");
    }

    return Multipoint::create(m_coordinate, m_locations, m_values, m_derivatives);
}

// ============================================================================
// IMPLEMENTATION - Factory methods
// ============================================================================

template<typename C, typename I>
CubicSpline<C, I>* CubicSpline<C, I>::constant(float value) {
    return new Constant(value);
}

template<typename C, typename I>
typename CubicSpline<C, I>::Builder CubicSpline<C, I>::builder(I* coordinate) {
    return Builder(coordinate);
}

template<typename C, typename I>
typename CubicSpline<C, I>::Builder CubicSpline<C, I>::builder(
    I* coordinate,
    BoundedFloatFunction<float>* valueTransformer)
{
    return Builder(coordinate, valueTransformer);
}

// Implementation of BoundedFloatFunction static helpers
template<typename C>
BoundedFloatFunction<float>* BoundedFloatFunction<C>::createUnlimited(std::function<float(float)> function) {
    return new UnlimitedFloatFunction(function);
}

template<typename C>
BoundedFloatFunction<float>* BoundedFloatFunction<C>::identity() {
    static UnlimitedFloatFunction* identityFunc = new UnlimitedFloatFunction([](float x) { return x; });
    return identityFunc;
}

} // namespace util
} // namespace minecraft
