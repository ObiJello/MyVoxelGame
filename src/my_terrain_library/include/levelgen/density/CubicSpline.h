#pragma once

#include "levelgen/density/BoundedFloatFunction.h"
#include "levelgen/density/Interval.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Reference: util.CubicSpline (26.3) - a piecewise cubic (Hermite) spline over
// a coordinate function, whose point values are themselves splines. Sealed in
// Java to Multipoint and Constant; the port keeps both and Java's float math
// exactly.
//
// Java's CubicSpline<I> is generic over the coordinate type; the port erases
// it (as the JVM does) to BoundedFloatFunction. A spline built for
// SplineFunction holds SplineFunction::Coordinate coordinates; the compiled
// sampler maps them to its own coordinate type with mapCoordinates.

namespace minecraft {
namespace levelgen {
namespace density {

class CubicSpline : public std::enable_shared_from_this<CubicSpline> {
public:
    using Ptr = std::shared_ptr<const CubicSpline>;
    using CoordinateConsumer = std::function<void(const BoundedFloatFunctionPtr&)>;
    using CoordinateMapper = std::function<BoundedFloatFunctionPtr(const BoundedFloatFunctionPtr&)>;

    class Multipoint;
    class Constant;
    class Builder;

    virtual ~CubicSpline() = default;

    virtual void forEachCoordinate(const CoordinateConsumer& consumer) const = 0;
    virtual Ptr mapCoordinates(const CoordinateMapper& mapper) const = 0;
    virtual Interval range() const = 0;
    virtual std::string parityString() const = 0;

    // Record equality / hashCode (Multipoint overrides Java's with array equality).
    virtual bool equals(const CubicSpline& other) const = 0;
    virtual size_t hash() const = 0;

    // CubicSpline.sample(spline, coordinate).
    static float sample(const CubicSpline& spline, BoundedFloatFunction::Argument& coordinate);
    // CubicSpline.asSampler(spline).
    static BoundedFloatFunctionPtr asSampler(const Ptr& spline);
    // CubicSpline.constant(value).
    static Ptr constant(float value);
    // CubicSpline.builder(coordinate[, valueTransformer]).
    static Builder builder(BoundedFloatFunctionPtr coordinate);
    static Builder builder(BoundedFloatFunctionPtr coordinate, std::function<float(float)> valueTransformer);

    // The sealed-interface type switch (Multipoint or Constant).
    enum class Kind { MULTIPOINT, CONSTANT };
    Kind kind() const { return m_kind; }

protected:
    explicit CubicSpline(Kind kind) : m_kind(kind) {}

private:
    Kind m_kind;
};

// ---- CubicSpline.Multipoint ----------------------------------------------------

class CubicSpline::Multipoint final : public CubicSpline {
public:
    // Throws std::invalid_argument when the sizes differ or there are no points (validateSizes).
    Multipoint(BoundedFloatFunctionPtr coordinate, std::vector<float> locations, std::vector<Ptr> values,
               std::vector<float> derivatives);

    void forEachCoordinate(const CoordinateConsumer& consumer) const override;
    Ptr mapCoordinates(const CoordinateMapper& mapper) const override;
    Interval range() const override;
    std::string parityString() const override;
    bool equals(const CubicSpline& other) const override;
    size_t hash() const override;

    // Multipoint.sample(sampler, c).
    static float sample(const Multipoint& sampler, BoundedFloatFunction::Argument& c);

    const BoundedFloatFunctionPtr& coordinate() const { return m_coordinate; }
    const std::vector<float>& locations() const { return m_locations; }
    const std::vector<Ptr>& values() const { return m_values; }
    const std::vector<float>& derivatives() const { return m_derivatives; }

private:
    static float linearExtend(float input, const std::vector<float>& locations, float value,
                              const std::vector<float>& derivatives, int index);
    static void validateSizes(const std::vector<float>& locations, const std::vector<Ptr>& values,
                              const std::vector<float>& derivatives);
    static float sample(const BoundedFloatFunction& coordinate, const std::vector<float>& derivatives,
                        const std::vector<float>& locations, const std::vector<Ptr>& values,
                        BoundedFloatFunction::Argument& c);
    static int findIntervalStart(const std::vector<float>& locations, float input);

    BoundedFloatFunctionPtr m_coordinate;
    std::vector<float> m_locations;
    std::vector<Ptr> m_values;
    std::vector<float> m_derivatives;
};

// ---- CubicSpline.Constant ------------------------------------------------------

class CubicSpline::Constant final : public CubicSpline {
public:
    explicit Constant(float value) : CubicSpline(Kind::CONSTANT), m_value(value) {}

    void forEachCoordinate(const CoordinateConsumer&) const override {}
    Ptr mapCoordinates(const CoordinateMapper&) const override { return shared_from_this(); }
    Interval range() const override { return Interval::ofExact(m_value); }
    std::string parityString() const override;
    bool equals(const CubicSpline& other) const override;
    size_t hash() const override;

    float value() const { return m_value; }

private:
    float m_value;
};

// ---- CubicSpline.Builder -------------------------------------------------------

class CubicSpline::Builder {
public:
    explicit Builder(BoundedFloatFunctionPtr coordinate);
    Builder(BoundedFloatFunctionPtr coordinate, std::function<float(float)> valueTransformer);

    Builder& addPoint(float location, float value);
    Builder& addPoint(float location, float value, float derivative);
    Builder& addPoint(float location, Ptr sampler);
    // Throws std::logic_error when no point was added.
    Ptr build() const;

private:
    Builder& addPoint(float location, Ptr sampler, float derivative);

    BoundedFloatFunctionPtr m_coordinate;
    std::function<float(float)> m_valueTransformer;
    std::vector<float> m_locations;
    std::vector<Ptr> m_values;
    std::vector<float> m_derivatives;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
