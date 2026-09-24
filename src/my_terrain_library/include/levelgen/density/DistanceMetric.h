#pragma once

#include "levelgen/density/JavaMath.h"

#include <cmath>
#include <stdexcept>

// Reference: densityfunction.DistanceMetric (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

enum class DistanceMetric {
    EUCLIDEAN,
    EUCLIDEAN_SQUARED,
    MANHATTAN,
    CHEBYSHEV,
};

inline const char* serializedName(DistanceMetric metric) {
    switch (metric) {
        case DistanceMetric::EUCLIDEAN: return "euclidean";
        case DistanceMetric::EUCLIDEAN_SQUARED: return "euclidean_squared";
        case DistanceMetric::MANHATTAN: return "manhattan";
        case DistanceMetric::CHEBYSHEV: return "chebyshev";
    }
    throw std::logic_error("DistanceMetric: bad value");
}

// DistanceMetric.compute(deltaX, deltaY, deltaZ).
inline float compute(DistanceMetric metric, float deltaX, float deltaY, float deltaZ) {
    switch (metric) {
        case DistanceMetric::EUCLIDEAN: return jmath::length(deltaX, deltaY, deltaZ);
        case DistanceMetric::EUCLIDEAN_SQUARED: return jmath::lengthSquared(deltaX, deltaY, deltaZ);
        case DistanceMetric::MANHATTAN: return std::fabs(deltaX) + std::fabs(deltaY) + std::fabs(deltaZ);
        case DistanceMetric::CHEBYSHEV:
            return jmath::fmax(jmath::fmax(std::fabs(deltaX), std::fabs(deltaY)), std::fabs(deltaZ));
    }
    throw std::logic_error("DistanceMetric: bad value");
}

// DistanceMetric.compute(deltaX, deltaZ).
inline float compute(DistanceMetric metric, float deltaX, float deltaZ) {
    switch (metric) {
        case DistanceMetric::EUCLIDEAN: return jmath::length(deltaX, deltaZ);
        case DistanceMetric::EUCLIDEAN_SQUARED: return jmath::lengthSquared(deltaX, deltaZ);
        case DistanceMetric::MANHATTAN: return std::fabs(deltaX) + std::fabs(deltaZ);
        case DistanceMetric::CHEBYSHEV: return jmath::fmax(std::fabs(deltaX), std::fabs(deltaZ));
    }
    throw std::logic_error("DistanceMetric: bad value");
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
