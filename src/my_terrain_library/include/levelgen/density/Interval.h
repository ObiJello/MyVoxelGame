#pragma once

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

// Reference: net.minecraft.util.Interval (26.3) - a density function's value
// range, used at compile time to pick samplers (a min/max whose inputs never
// overlap collapses to one side, a clamp that cannot bind disappears). The
// arithmetic is float and must match Java's exactly.

namespace minecraft {
namespace levelgen {
namespace density {

class Interval {
public:
    // NaI ("not an interval"): the only interval with NaN bounds.
    static Interval NaI() { return Interval(std::numeric_limits<float>::quiet_NaN(),
                                            std::numeric_limits<float>::quiet_NaN()); }
    static Interval infinite() { return Interval(-std::numeric_limits<float>::infinity(),
                                                 std::numeric_limits<float>::infinity()); }

    // Throws std::invalid_argument for max < min or a NaN bound.
    static Interval of(float min, float max);
    static Interval ofSymmetric(float range) { return of(-range, range); }
    static Interval ofExact(float value) { return of(value, value); }

    static Interval encapsulating(const std::vector<Interval>& intervals);
    static Interval encapsulating(float first, float second);

    static Interval add(const Interval& left, const Interval& right);
    static Interval sub(const Interval& left, const Interval& right);
    static Interval mul(const Interval& left, const Interval& right);
    static Interval reciprocal(const Interval& input);
    static Interval div(const Interval& left, const Interval& right);
    static Interval min(const Interval& left, const Interval& right);
    static Interval max(const Interval& left, const Interval& right);
    static Interval clamp(const Interval& input, float min, float max);
    static Interval abs(const Interval& input);
    static Interval square(const Interval& input);
    static Interval pow(const Interval& base, const Interval& exponent);
    static Interval log(const Interval& input);
    static Interval mapMonotonic(const Interval& input, const std::function<float(float)>& op);
    static Interval lerp(const Interval& alpha, const Interval& first, const Interval& second);
    static Interval lerp(const Interval& alpha, float first, float second);
    static Interval sign(const Interval& input);

    bool contains(float value) const { return value >= m_min && value <= m_max; }
    bool intersects(const Interval& other) const { return m_min <= other.m_max && m_max >= other.m_min; }
    bool isNaI() const { return std::isnan(m_min); }
    float min() const { return m_min; }
    float max() const { return m_max; }

    bool operator==(const Interval& other) const {
        return (isNaI() && other.isNaI()) || (m_min == other.m_min && m_max == other.m_max);
    }
    bool operator!=(const Interval& other) const { return !(*this == other); }

    std::string toString() const;

private:
    Interval(float min, float max) : m_min(min), m_max(max) {}

    static Interval encapsulating(const Interval& first, float second);
    static float mulBound(float left, float right);
    static Interval powScalar(float base, const Interval& exponent);
    static Interval powPositiveBase(float base, const Interval& exponent);
    static Interval powZeroBase(const Interval& exponent);
    static Interval powInfiniteExponent(float base, const Interval& exponent);
    static Interval powNegativeBase(float base, const Interval& exponent);
    static Interval lerpFiniteBounds(const Interval& alpha, float first, float second);
    static float lerpFiniteBound(float alpha, float first, float second);
    static Interval lerpInfiniteBounds(const Interval& alpha, float first, float second);
    static float lerpInfiniteBound(float alpha, float first, float second);

    float m_min;
    float m_max;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
