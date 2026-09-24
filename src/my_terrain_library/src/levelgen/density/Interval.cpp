#include "levelgen/density/Interval.h"
#include "levelgen/density/JavaMath.h"

#include <stdexcept>

// Reference: net.minecraft.util.Interval (26.3), method for method.

namespace minecraft {
namespace levelgen {
namespace density {

using jmath::fmax;
using jmath::fmin;

namespace {
const float kInf = std::numeric_limits<float>::infinity();
}

Interval Interval::of(float min, float max) {
    if (max < min) {
        throw std::invalid_argument("max (" + std::to_string(max) + ") < min (" + std::to_string(min) + ")");
    }
    if (std::isnan(min) || std::isnan(max)) {
        throw std::invalid_argument("Bounds cannot include NaN: use Interval::NaI explicitly");
    }
    return Interval(min, max);
}

Interval Interval::encapsulating(const std::vector<Interval>& intervals) {
    if (intervals.empty()) {
        throw std::invalid_argument("At least one interval required");
    }
    float min = kInf;
    float max = -kInf;
    for (const Interval& interval : intervals) {
        if (!interval.isNaI()) {
            min = fmin(interval.m_min, min);
            max = fmax(interval.m_max, max);
        }
    }
    return max < min ? NaI() : of(min, max);
}

Interval Interval::encapsulating(float first, float second) {
    if (std::isnan(first) && std::isnan(second)) return NaI();
    if (std::isnan(first)) return ofExact(second);
    return std::isnan(second) ? ofExact(first) : of(fmin(first, second), fmax(first, second));
}

Interval Interval::encapsulating(const Interval& first, float second) {
    if (std::isnan(second)) return first;
    return first.isNaI() ? ofExact(second) : of(fmin(first.min(), second), fmax(first.max(), second));
}

Interval Interval::add(const Interval& left, const Interval& right) {
    const float min = left.m_min + right.m_min;
    const float max = left.m_max + right.m_max;
    return !std::isnan(min) && !std::isnan(max) ? of(min, max) : NaI();
}

Interval Interval::sub(const Interval& left, const Interval& right) {
    const float min = left.m_min - right.m_max;
    const float max = left.m_max - right.m_min;
    return !std::isnan(min) && !std::isnan(max) ? of(min, max) : NaI();
}

float Interval::mulBound(float left, float right) {
    return left != 0.0f && right != 0.0f ? left * right : 0.0f;
}

Interval Interval::mul(const Interval& left, const Interval& right) {
    if (left.isNaI() || right.isNaI()) return NaI();
    const float minMin = mulBound(left.m_min, right.m_min);
    const float minMax = mulBound(left.m_min, right.m_max);
    const float maxMin = mulBound(left.m_max, right.m_min);
    const float maxMax = mulBound(left.m_max, right.m_max);
    return of(fmin(fmin(minMin, minMax), fmin(maxMin, maxMax)),
              fmax(fmax(minMin, minMax), fmax(maxMin, maxMax)));
}

Interval Interval::reciprocal(const Interval& input) {
    if (input.isNaI() || (input.m_min == 0.0f && input.m_max == 0.0f)) return NaI();
    if (!input.contains(0.0f)) return of(1.0f / input.m_max, 1.0f / input.m_min);
    if (input.m_max == 0.0f) return of(-kInf, 1.0f / input.m_min);
    return input.m_min == 0.0f ? of(1.0f / input.m_max, kInf) : infinite();
}

Interval Interval::div(const Interval& left, const Interval& right) {
    return mul(left, reciprocal(right));
}

Interval Interval::min(const Interval& left, const Interval& right) {
    return !left.isNaI() && !right.isNaI()
        ? of(fmin(left.m_min, right.m_min), fmin(left.m_max, right.m_max)) : NaI();
}

Interval Interval::max(const Interval& left, const Interval& right) {
    return !left.isNaI() && !right.isNaI()
        ? of(fmax(left.m_min, right.m_min), fmax(left.m_max, right.m_max)) : NaI();
}

Interval Interval::clamp(const Interval& input, float min, float max) {
    if (min > max) {
        throw std::invalid_argument("min > max in Interval::clamp");
    }
    if (input.isNaI()) return NaI();
    if (input.m_min >= max) return of(max, max);
    return input.m_max <= min ? of(min, min) : of(fmax(input.m_min, min), fmin(input.m_max, max));
}

Interval Interval::abs(const Interval& input) {
    if (input.isNaI()) return NaI();
    const float max = fmax(std::fabs(input.m_min), std::fabs(input.m_max));
    return input.contains(0.0f) ? of(0.0f, max)
                                : of(fmin(std::fabs(input.m_min), std::fabs(input.m_max)), max);
}

Interval Interval::square(const Interval& input) {
    if (input.isNaI()) return NaI();
    const float max = fmax(jmath::square(input.m_min), jmath::square(input.m_max));
    return input.contains(0.0f) ? of(0.0f, max)
                                : of(fmin(jmath::square(input.m_min), jmath::square(input.m_max)), max);
}

Interval Interval::pow(const Interval& base, const Interval& exponent) {
    if (base.isNaI() || exponent.isNaI()) return NaI();
    if (base.min() == base.max()) return powScalar(base.min(), exponent);
    Interval result = encapsulating(std::vector<Interval>{powScalar(base.min(), exponent),
                                                          powScalar(base.max(), exponent)});
    if (base.contains(0.0f)) {
        if (base.max() > 0.0f) result = encapsulating(std::vector<Interval>{result, powScalar(0.0f, exponent)});
        if (base.min() < 0.0f) result = encapsulating(std::vector<Interval>{result, powScalar(-0.0f, exponent)});
    }
    return result;
}

Interval Interval::powScalar(float base, const Interval& exponent) {
    if (std::isnan(base) || exponent.isNaI()) return NaI();
    if (exponent.min() == exponent.max()) {
        const float value = static_cast<float>(std::pow(static_cast<double>(base),
                                                        static_cast<double>(exponent.min())));
        return std::isnan(value) ? NaI() : ofExact(value);
    }
    if (base == 0.0f) return mul(powZeroBase(exponent), ofExact(std::copysign(1.0f, base)));
    if (base == 1.0f) return ofExact(1.0f);
    return base > 0.0f ? powPositiveBase(base, exponent) : powNegativeBase(base, exponent);
}

Interval Interval::powPositiveBase(float base, const Interval& exponent) {
    return std::isfinite(exponent.min()) && std::isfinite(exponent.max())
        ? encapsulating(static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent.min()))),
                        static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent.max()))))
        : powInfiniteExponent(base, exponent);
}

Interval Interval::powZeroBase(const Interval& exponent) {
    if (exponent.contains(0.0f)) {
        if (exponent.max() == 0.0f) return of(1.0f, kInf);
        return exponent.min() == 0.0f ? of(0.0f, 1.0f) : of(0.0f, kInf);
    }
    return exponent.max() < 0.0f ? ofExact(kInf) : ofExact(0.0f);
}

Interval Interval::powInfiniteExponent(float base, const Interval& exponent) {
    if (std::isinf(exponent.min()) && std::isinf(exponent.max())) return of(0.0f, kInf);
    if (std::isinf(exponent.min())) {
        const float p = static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent.max())));
        return base < 1.0f ? of(p, kInf) : of(0.0f, p);
    }
    const float p = static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponent.min())));
    return base < 1.0f ? of(0.0f, p) : of(p, kInf);
}

Interval Interval::powNegativeBase(float base, const Interval& exponent) {
    const float exponentMinInt = static_cast<float>(std::ceil(static_cast<double>(exponent.min())));
    const float exponentMaxInt = static_cast<float>(std::floor(static_cast<double>(exponent.max())));
    if (exponentMaxInt < exponentMinInt) return NaI();
    const float baseToMinInt = static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponentMinInt)));
    const float baseToMaxInt = static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(exponentMaxInt)));
    Interval result = encapsulating(baseToMinInt, baseToMaxInt);
    if (std::isinf(exponentMinInt)) {
        result = encapsulating(result, -baseToMinInt);
    } else if (exponentMinInt + 1.0f < exponentMaxInt) {
        result = encapsulating(result, static_cast<float>(std::pow(static_cast<double>(base),
                                                                   static_cast<double>(exponentMinInt + 1.0f))));
    }
    if (std::isinf(exponentMaxInt)) {
        result = encapsulating(result, -baseToMaxInt);
    } else if (exponentMaxInt - 1.0f > exponentMinInt) {
        result = encapsulating(result, static_cast<float>(std::pow(static_cast<double>(base),
                                                                   static_cast<double>(exponentMaxInt - 1.0f))));
    }
    return result;
}

Interval Interval::log(const Interval& input) {
    if (input.max() < 0.0f) return NaI();
    const Interval clippedInput = max(input, ofExact(0.0f));
    return mapMonotonic(clippedInput, [](float x) {
        return static_cast<float>(std::log(static_cast<double>(x)));
    });
}

Interval Interval::mapMonotonic(const Interval& input, const std::function<float(float)>& op) {
    if (input.isNaI()) return NaI();
    const float mappedMin = op(input.m_min);
    const float mappedMax = op(input.m_max);
    if (std::isnan(mappedMin) || std::isnan(mappedMax)) {
        throw std::logic_error("Monotonic operator should not produce NaN");
    }
    return of(fmin(mappedMin, mappedMax), fmax(mappedMin, mappedMax));
}

Interval Interval::lerp(const Interval& alpha, const Interval& first, const Interval& second) {
    if (alpha.isNaI() || first.isNaI() || second.isNaI()) return NaI();
    return encapsulating(std::vector<Interval>{lerp(alpha, first.m_min, second.m_min),
                                               lerp(alpha, first.m_max, second.m_min),
                                               lerp(alpha, first.m_min, second.m_max),
                                               lerp(alpha, first.m_max, second.m_max)});
}

Interval Interval::lerp(const Interval& alpha, float first, float second) {
    if (alpha.isNaI() || std::isnan(first) || std::isnan(second)) return NaI();
    return std::isfinite(first) && std::isfinite(second) ? lerpFiniteBounds(alpha, first, second)
                                                         : lerpInfiniteBounds(alpha, first, second);
}

Interval Interval::lerpFiniteBounds(const Interval& alpha, float first, float second) {
    return encapsulating(lerpFiniteBound(alpha.m_min, first, second), lerpFiniteBound(alpha.m_max, first, second));
}

float Interval::lerpFiniteBound(float alpha, float first, float second) {
    return first + mulBound(alpha, second - first);
}

Interval Interval::lerpInfiniteBounds(const Interval& alpha, float first, float second) {
    if (first == second) return ofExact(first);
    const float newMin = lerpInfiniteBound(alpha.m_min, first, second);
    const float newMax = lerpInfiniteBound(alpha.m_max, first, second);
    return !std::isnan(newMin) && !std::isnan(newMax) ? encapsulating(newMin, newMax) : NaI();
}

float Interval::lerpInfiniteBound(float alpha, float first, float second) {
    const float firstPart = mulBound(1.0f - alpha, first);
    const float secondPart = mulBound(alpha, second);
    if (std::isinf(firstPart) && std::isinf(secondPart)) {
        if (alpha <= 0.0f) return second > first ? -kInf : kInf;
        if (alpha >= 1.0f) return second > first ? kInf : -kInf;
        return std::numeric_limits<float>::quiet_NaN();
    }
    return firstPart + secondPart;
}

Interval Interval::sign(const Interval& input) {
    if (input.isNaI()) return NaI();
    if (input.min() == input.max()) return ofExact(jmath::signum(input.min()));
    if (input.contains(0.0f)) {
        if (input.min() == 0.0f) return of(0.0f, 1.0f);
        return input.max() == 0.0f ? of(-1.0f, 0.0f) : of(-1.0f, 1.0f);
    }
    return ofExact(input.min() > 0.0f ? 1.0f : -1.0f);
}

std::string Interval::toString() const {
    return isNaI() ? "[NaN]" : "[" + std::to_string(m_min) + "; " + std::to_string(m_max) + "]";
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
