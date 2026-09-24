#pragma once

#include "levelgen/density/Interval.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Reference: util.BoundedFloatFunction (26.3) - a float function of some
// context C with a known value range; the coordinate type of a CubicSpline.
//
// Java erases C at runtime and so does the port: apply() takes the caller's
// context as a BoundedFloatFunction::Argument, which each caller derives its
// concrete C from (SplineFunction's SplineInput). A function whose C carries
// nothing (SplineFunction::Coordinate, C = Unit) ignores it.

namespace minecraft {
namespace levelgen {
namespace density {

class BoundedFloatFunction {
public:
    // The erased C.
    class Argument {
    public:
        virtual ~Argument() = default;
    };

    virtual ~BoundedFloatFunction() = default;

    virtual float apply(Argument& c) const = 0;
    virtual Interval range() const = 0;

    // Object.equals/hashCode: identity unless the implementation is a record.
    virtual bool equals(const BoundedFloatFunction& other) const { return this == &other; }
    virtual size_t hash() const { return std::hash<const void*>()(this); }
    // String.valueOf(this), for CubicSpline.parityString().
    virtual std::string toString() const { return "BoundedFloatFunction"; }

    // BoundedFloatFunction.constant(value).
    static std::shared_ptr<const BoundedFloatFunction> constant(float value);
};

using BoundedFloatFunctionPtr = std::shared_ptr<const BoundedFloatFunction>;

inline std::shared_ptr<const BoundedFloatFunction> BoundedFloatFunction::constant(float value) {
    class ConstantBoundedFloatFunction final : public BoundedFloatFunction {
    public:
        explicit ConstantBoundedFloatFunction(float value) : m_value(value), m_range(Interval::ofExact(value)) {}
        float apply(Argument&) const override { return m_value; }
        Interval range() const override { return m_range; }
    private:
        float m_value;
        Interval m_range;
    };
    return std::make_shared<ConstantBoundedFloatFunction>(value);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
