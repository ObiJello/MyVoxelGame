#include "levelgen/density/CubicSpline.h"

#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// Reference: util.CubicSpline (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// String.format(Locale.ROOT, "%.3f", value): Java rounds the shortest decimal
// digits of the double HALF_UP (C's printf rounds the exact binary value).
std::string formatJava3f(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0.0 ? "Infinity" : "-Infinity";
    const bool negative = std::signbit(value);
    const double magnitude = std::fabs(value);

    std::string digits;
    int integerDigits;  // digits before the decimal point
    if (magnitude == 0.0) {
        digits = "0";
        integerDigits = 1;
    } else {
        char buffer[64];
        for (int precision = 1; precision <= 17; ++precision) {
            std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, magnitude);
            if (std::strtod(buffer, nullptr) == magnitude) break;
        }
        const char* exponent = std::strchr(buffer, 'e');
        for (const char* p = buffer; p != exponent; ++p) {
            if (*p != '.') digits.push_back(*p);
        }
        integerDigits = std::atoi(exponent + 1) + 1;
    }

    std::string integerPart;
    std::string fraction;
    if (integerDigits <= 0) {
        integerPart = "0";
        fraction = std::string(static_cast<size_t>(-integerDigits), '0') + digits;
    } else if (static_cast<size_t>(integerDigits) >= digits.size()) {
        integerPart = digits + std::string(static_cast<size_t>(integerDigits) - digits.size(), '0');
    } else {
        integerPart = digits.substr(0, static_cast<size_t>(integerDigits));
        fraction = digits.substr(static_cast<size_t>(integerDigits));
    }
    const bool roundUp = fraction.size() > 3 && fraction[3] >= '5';
    fraction.resize(3, '0');
    std::string number = integerPart + fraction;
    if (roundUp) {
        int i = static_cast<int>(number.size()) - 1;
        while (i >= 0 && number[static_cast<size_t>(i)] == '9') {
            number[static_cast<size_t>(i)] = '0';
            --i;
        }
        if (i >= 0) {
            ++number[static_cast<size_t>(i)];
        } else {
            number.insert(number.begin(), '1');
        }
    }
    std::string result = number.substr(0, number.size() - 3) + "." + number.substr(number.size() - 3);
    return negative ? "-" + result : result;
}

std::string arrayToString(const std::vector<float>& values) {
    std::string result = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) result += ", ";
        result += formatJava3f(static_cast<double>(values[i]));
    }
    return result + "]";
}

// Arrays.equals(float[], float[]): floatToIntBits equality.
bool floatArraysEqual(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!floatEq(a[i], b[i])) return false;
    }
    return true;
}

// The BoundedFloatFunction asSampler returns for a Multipoint.
class MultipointSampler final : public BoundedFloatFunction {
public:
    explicit MultipointSampler(std::shared_ptr<const CubicSpline::Multipoint> multipoint)
        : m_multipoint(std::move(multipoint)) {}
    float apply(Argument& c) const override { return CubicSpline::Multipoint::sample(*m_multipoint, c); }
    Interval range() const override { return m_multipoint->range(); }
private:
    std::shared_ptr<const CubicSpline::Multipoint> m_multipoint;
};

} // namespace

// ---- CubicSpline ---------------------------------------------------------------

float CubicSpline::sample(const CubicSpline& spline, BoundedFloatFunction::Argument& coordinate) {
    switch (spline.kind()) {
        case Kind::MULTIPOINT:
            return Multipoint::sample(static_cast<const Multipoint&>(spline), coordinate);
        case Kind::CONSTANT:
            return static_cast<const Constant&>(spline).value();
    }
    throw std::logic_error("CubicSpline: unknown spline kind");
}

BoundedFloatFunctionPtr CubicSpline::asSampler(const Ptr& spline) {
    switch (spline->kind()) {
        case Kind::MULTIPOINT:
            return std::make_shared<MultipointSampler>(std::static_pointer_cast<const Multipoint>(spline));
        case Kind::CONSTANT:
            return BoundedFloatFunction::constant(static_cast<const Constant&>(*spline).value());
    }
    throw std::logic_error("CubicSpline: unknown spline kind");
}

CubicSpline::Ptr CubicSpline::constant(float value) {
    return std::make_shared<Constant>(value);
}

CubicSpline::Builder CubicSpline::builder(BoundedFloatFunctionPtr coordinate) {
    return Builder(std::move(coordinate));
}

CubicSpline::Builder CubicSpline::builder(BoundedFloatFunctionPtr coordinate,
                                          std::function<float(float)> valueTransformer) {
    return Builder(std::move(coordinate), std::move(valueTransformer));
}

// ---- Multipoint ----------------------------------------------------------------

CubicSpline::Multipoint::Multipoint(BoundedFloatFunctionPtr coordinate, std::vector<float> locations,
                                    std::vector<Ptr> values, std::vector<float> derivatives)
    : CubicSpline(Kind::MULTIPOINT) {
    validateSizes(locations, values, derivatives);
    m_coordinate = std::move(coordinate);
    m_locations = std::move(locations);
    m_values = std::move(values);
    m_derivatives = std::move(derivatives);
}

Interval CubicSpline::Multipoint::range() const {
    const int lastIndex = static_cast<int>(m_locations.size()) - 1;
    float minValue = std::numeric_limits<float>::infinity();
    float maxValue = -std::numeric_limits<float>::infinity();
    const Interval inputRange = m_coordinate->range();
    if (inputRange.isNaI()) {
        return inputRange;
    }
    if (inputRange.min() < m_locations[0]) {
        const Interval lastRange = m_values.front()->range();
        const float edge1 = linearExtend(inputRange.min(), m_locations, lastRange.min(), m_derivatives, 0);
        const float edge2 = linearExtend(inputRange.min(), m_locations, lastRange.max(), m_derivatives, 0);
        minValue = jmath::fmin(minValue, jmath::fmin(edge1, edge2));
        maxValue = jmath::fmax(maxValue, jmath::fmax(edge1, edge2));
    }
    if (inputRange.max() > m_locations[static_cast<size_t>(lastIndex)]) {
        const Interval lastRange = m_values[static_cast<size_t>(lastIndex)]->range();
        const float edge1 = linearExtend(inputRange.max(), m_locations, lastRange.min(), m_derivatives, lastIndex);
        const float edge2 = linearExtend(inputRange.max(), m_locations, lastRange.max(), m_derivatives, lastIndex);
        minValue = jmath::fmin(minValue, jmath::fmin(edge1, edge2));
        maxValue = jmath::fmax(maxValue, jmath::fmax(edge1, edge2));
    }

    std::vector<Interval> valueRanges;
    valueRanges.reserve(m_values.size());
    for (const Ptr& value : m_values) {
        valueRanges.push_back(value->range());
    }
    for (const Interval& range : valueRanges) {
        minValue = jmath::fmin(minValue, range.min());
        maxValue = jmath::fmax(maxValue, range.max());
    }

    for (int i = 0; i < lastIndex; ++i) {
        const float x1 = m_locations[static_cast<size_t>(i)];
        const float x2 = m_locations[static_cast<size_t>(i + 1)];
        const float xDiff = x2 - x1;
        const Interval& range1 = valueRanges[static_cast<size_t>(i)];
        const Interval& range2 = valueRanges[static_cast<size_t>(i + 1)];
        const float min1 = range1.min();
        const float max1 = range1.max();
        const float min2 = range2.min();
        const float max2 = range2.max();
        const float d1 = m_derivatives[static_cast<size_t>(i)];
        const float d2 = m_derivatives[static_cast<size_t>(i + 1)];
        if (d1 != 0.0f || d2 != 0.0f) {
            const float p1 = d1 * xDiff;
            const float p2 = d2 * xDiff;
            const float minLerp1 = jmath::fmin(min1, min2);
            const float maxLerp1 = jmath::fmax(max1, max2);
            const float minA = p1 - max2 + min1;
            const float maxA = p1 - min2 + max1;
            const float minB = -p2 + min2 - max1;
            const float maxB = -p2 + max2 - min1;
            const float minLerp2 = jmath::fmin(minA, minB);
            const float maxLerp2 = jmath::fmax(maxA, maxB);
            minValue = jmath::fmin(minValue, minLerp1 + 0.25f * minLerp2);
            maxValue = jmath::fmax(maxValue, maxLerp1 + 0.25f * maxLerp2);
        }
    }

    return Interval::of(minValue, maxValue);
}

float CubicSpline::Multipoint::linearExtend(float input, const std::vector<float>& locations, float value,
                                            const std::vector<float>& derivatives, int index) {
    const float derivative = derivatives[static_cast<size_t>(index)];
    return derivative == 0.0f ? value : value + derivative * (input - locations[static_cast<size_t>(index)]);
}

void CubicSpline::Multipoint::validateSizes(const std::vector<float>& locations, const std::vector<Ptr>& values,
                                            const std::vector<float>& derivatives) {
    if (locations.size() == values.size() && locations.size() == derivatives.size()) {
        if (locations.empty()) {
            throw std::invalid_argument("Cannot create a multipoint spline with no points");
        }
    } else {
        throw std::invalid_argument("All lengths must be equal, got: " + std::to_string(locations.size()) + " " +
                                    std::to_string(values.size()) + " " + std::to_string(derivatives.size()));
    }
}

float CubicSpline::Multipoint::sample(const Multipoint& sampler, BoundedFloatFunction::Argument& c) {
    return sample(*sampler.m_coordinate, sampler.m_derivatives, sampler.m_locations, sampler.m_values, c);
}

float CubicSpline::Multipoint::sample(const BoundedFloatFunction& coordinate, const std::vector<float>& derivatives,
                                      const std::vector<float>& locations, const std::vector<Ptr>& values,
                                      BoundedFloatFunction::Argument& c) {
    const float input = coordinate.apply(c);
    const int start = findIntervalStart(locations, input);
    const int lastIndex = static_cast<int>(locations.size()) - 1;
    if (start < 0) {
        const float value = CubicSpline::sample(*values.front(), c);
        return linearExtend(input, locations, value, derivatives, 0);
    }
    if (start == lastIndex) {
        const float value = CubicSpline::sample(*values[static_cast<size_t>(lastIndex)], c);
        return linearExtend(input, locations, value, derivatives, lastIndex);
    }
    const float x1 = locations[static_cast<size_t>(start)];
    const float x2 = locations[static_cast<size_t>(start + 1)];
    const float t = (input - x1) / (x2 - x1);
    const CubicSpline& f1 = *values[static_cast<size_t>(start)];
    const CubicSpline& f2 = *values[static_cast<size_t>(start + 1)];
    const float d1 = derivatives[static_cast<size_t>(start)];
    const float d2 = derivatives[static_cast<size_t>(start + 1)];
    const float y1 = CubicSpline::sample(f1, c);
    const float y2 = CubicSpline::sample(f2, c);
    const float a = d1 * (x2 - x1) - (y2 - y1);
    const float b = -d2 * (x2 - x1) + (y2 - y1);
    const float offset = jmath::lerp(t, y1, y2) + t * (1.0f - t) * jmath::lerp(t, a, b);
    return offset;
}

int CubicSpline::Multipoint::findIntervalStart(const std::vector<float>& locations, float input) {
    // Mth.binarySearch(0, locations.length, i -> input < locations[i]) - 1.
    int from = 0;
    int len = static_cast<int>(locations.size());
    while (len > 0) {
        const int half = len / 2;
        const int middle = from + half;
        if (input < locations[static_cast<size_t>(middle)]) {
            len = half;
        } else {
            from = middle + 1;
            len -= half + 1;
        }
    }
    return from - 1;
}

std::string CubicSpline::Multipoint::parityString() const {
    std::string values = "[";
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (i > 0) values += ", ";
        values += m_values[i]->parityString();
    }
    values += "]";
    return "Spline{coordinate=" + m_coordinate->toString() + ", locations=" + arrayToString(m_locations) +
           ", derivatives=" + arrayToString(m_derivatives) + ", values=" + values + "}";
}

void CubicSpline::Multipoint::forEachCoordinate(const CoordinateConsumer& consumer) const {
    consumer(m_coordinate);
    for (const Ptr& spline : m_values) {
        spline->forEachCoordinate(consumer);
    }
}

CubicSpline::Ptr CubicSpline::Multipoint::mapCoordinates(const CoordinateMapper& mapper) const {
    // The coordinate maps first, then the values in order (SplineFunction's
    // sampler numbers its coordinates in this order).
    BoundedFloatFunctionPtr coordinate = mapper(m_coordinate);
    std::vector<Ptr> values;
    values.reserve(m_values.size());
    for (const Ptr& value : m_values) {
        values.push_back(value->mapCoordinates(mapper));
    }
    return std::make_shared<Multipoint>(std::move(coordinate), m_locations, std::move(values), m_derivatives);
}

bool CubicSpline::Multipoint::equals(const CubicSpline& other) const {
    if (other.kind() != Kind::MULTIPOINT) return false;
    const auto& multipoint = static_cast<const Multipoint&>(other);
    if (this == &multipoint) return true;
    if (!m_coordinate->equals(*multipoint.m_coordinate)) return false;
    if (!floatArraysEqual(m_locations, multipoint.m_locations)) return false;
    if (!floatArraysEqual(m_derivatives, multipoint.m_derivatives)) return false;
    if (m_values.size() != multipoint.m_values.size()) return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*multipoint.m_values[i])) return false;
    }
    return true;
}

size_t CubicSpline::Multipoint::hash() const {
    size_t h = hashCombine(0x51, m_coordinate->hash());
    for (float location : m_locations) h = hashCombine(h, hashFloat(location));
    for (const Ptr& value : m_values) h = hashCombine(h, value->hash());
    for (float derivative : m_derivatives) h = hashCombine(h, hashFloat(derivative));
    return h;
}

// ---- Constant ------------------------------------------------------------------

std::string CubicSpline::Constant::parityString() const {
    return "k=" + formatJava3f(static_cast<double>(m_value));
}

bool CubicSpline::Constant::equals(const CubicSpline& other) const {
    return other.kind() == Kind::CONSTANT && floatEq(m_value, static_cast<const Constant&>(other).m_value);
}

size_t CubicSpline::Constant::hash() const {
    return hashCombine(0x52, hashFloat(m_value));
}

// ---- Builder -------------------------------------------------------------------

CubicSpline::Builder::Builder(BoundedFloatFunctionPtr coordinate)
    : Builder(std::move(coordinate), [](float value) { return value; }) {}

CubicSpline::Builder::Builder(BoundedFloatFunctionPtr coordinate, std::function<float(float)> valueTransformer)
    : m_coordinate(std::move(coordinate)), m_valueTransformer(std::move(valueTransformer)) {}

CubicSpline::Builder& CubicSpline::Builder::addPoint(float location, float value) {
    return addPoint(location, std::make_shared<Constant>(m_valueTransformer(value)), 0.0f);
}

CubicSpline::Builder& CubicSpline::Builder::addPoint(float location, float value, float derivative) {
    return addPoint(location, std::make_shared<Constant>(m_valueTransformer(value)), derivative);
}

CubicSpline::Builder& CubicSpline::Builder::addPoint(float location, Ptr sampler) {
    return addPoint(location, std::move(sampler), 0.0f);
}

CubicSpline::Builder& CubicSpline::Builder::addPoint(float location, Ptr sampler, float derivative) {
    if (!m_locations.empty() && location <= m_locations.back()) {
        throw std::invalid_argument("Please register points in ascending order");
    }
    m_locations.push_back(location);
    m_values.push_back(std::move(sampler));
    m_derivatives.push_back(derivative);
    return *this;
}

CubicSpline::Ptr CubicSpline::Builder::build() const {
    if (m_locations.empty()) {
        throw std::logic_error("No elements added");
    }
    return std::make_shared<Multipoint>(m_coordinate, m_locations, m_values, m_derivatives);
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
