#include "levelgen/density/DensityFunctions.h"

#include "levelgen/density/generator/DistanceToPointFunction.h"
#include "levelgen/density/generator/EndIslandFunction.h"
#include "levelgen/density/generator/GradientFunction.h"
#include "levelgen/density/generator/NoiseFunction.h"
#include "levelgen/density/generator/ShiftNoiseFunction.h"
#include "levelgen/density/generator/SimpleDensityFunction.h"
#include "levelgen/density/op/BinaryFunction.h"
#include "levelgen/density/op/ClampFunction.h"
#include "levelgen/density/op/PowFunction.h"
#include "levelgen/density/op/UnaryFunction.h"

// Reference: densityfunction.DensityFunctions (26.3). The factories of the
// op functions LerpFunction, RangeChoiceFunction, IntervalSelectFunction,
// InterpolatedFunction, SplineFunction, FindTopSurfaceFunction and
// BlendDensityFunction are defined next to those classes.

namespace minecraft {
namespace levelgen {
namespace density {
namespace DensityFunctions {

DensityFunctionPtr abs(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::ABS, std::move(input));
}

DensityFunctionPtr square(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::SQUARE, std::move(input));
}

DensityFunctionPtr cube(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::CUBE, std::move(input));
}

DensityFunctionPtr sqrt(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::SQRT, std::move(input));
}

DensityFunctionPtr halfNegative(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::HALF_NEGATIVE, std::move(input));
}

DensityFunctionPtr quarterNegative(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::QUARTER_NEGATIVE, std::move(input));
}

DensityFunctionPtr reciprocal(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::RECIPROCAL, std::move(input));
}

DensityFunctionPtr negate(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::NEGATE, std::move(input));
}

DensityFunctionPtr squeeze(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::SQUEEZE, std::move(input));
}

DensityFunctionPtr log(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::LOG, std::move(input));
}

DensityFunctionPtr sign(DensityFunctionPtr input) {
    return std::make_shared<UnaryFunction>(UnaryFunction::Type::SIGN, std::move(input));
}

DensityFunctionPtr cache(DensityFunctionPtr function) {
    return std::make_shared<CacheFunction>(std::move(function));
}

DensityFunctionPtr mappedNoise(NoiseHolder noiseData, double xzScale, double yScale, float minTarget, float maxTarget) {
    DensityFunctionPtr noise =
        std::make_shared<NoiseFunction>(std::move(noiseData), xzScale, yScale, zero(), zero(), zero());
    return remap(noise, -1.0f, 1.0f, minTarget, maxTarget);
}

DensityFunctionPtr mappedNoise(NoiseHolder noiseData, double yScale, float minTarget, float maxTarget) {
    return mappedNoise(std::move(noiseData), 1.0, yScale, minTarget, maxTarget);
}

DensityFunctionPtr mappedNoise(NoiseHolder noiseData, float minTarget, float maxTarget) {
    return mappedNoise(std::move(noiseData), 1.0, 1.0, minTarget, maxTarget);
}

DensityFunctionPtr shiftedNoise2d(DensityFunctionPtr shiftX, DensityFunctionPtr shiftZ, double xzScale,
                                  NoiseHolder noiseData) {
    return std::make_shared<NoiseFunction>(std::move(noiseData), xzScale, 0.0, std::move(shiftX), zero(),
                                           std::move(shiftZ));
}

DensityFunctionPtr noise(NoiseHolder noiseData) {
    return noise(std::move(noiseData), 1.0, 1.0);
}

DensityFunctionPtr noise(NoiseHolder noiseData, double xzScale, double yScale) {
    return std::make_shared<NoiseFunction>(std::move(noiseData), xzScale, yScale, zero(), zero(), zero());
}

DensityFunctionPtr noise(NoiseHolder noiseData, double yScale) {
    return noise(std::move(noiseData), 1.0, yScale);
}

DensityFunctionPtr shiftA(NoiseHolder noiseData) {
    return std::make_shared<ShiftNoiseFunction::ShiftA>(std::move(noiseData));
}

DensityFunctionPtr shiftB(NoiseHolder noiseData) {
    return std::make_shared<ShiftNoiseFunction::ShiftB>(std::move(noiseData));
}

DensityFunctionPtr shift(NoiseHolder noiseData) {
    return std::make_shared<ShiftNoiseFunction::Shift>(std::move(noiseData));
}

DensityFunctionPtr endOuterIslands() {
    return std::make_shared<EndIslandFunction>();
}

DensityFunctionPtr distanceToPoint(const core::Vec3i& point, DistanceMetric metric) {
    return std::make_shared<DistanceToPointFunction>(point, metric);
}

DensityFunctionPtr add(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::ADD, std::move(left), std::move(right));
}

DensityFunctionPtr sub(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::SUB, std::move(left), std::move(right));
}

DensityFunctionPtr mul(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::MUL, std::move(left), std::move(right));
}

DensityFunctionPtr div(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::DIV, std::move(left), std::move(right));
}

DensityFunctionPtr pow(DensityFunctionPtr base, DensityFunctionPtr exponent) {
    return std::make_shared<PowFunction>(std::move(base), std::move(exponent));
}

DensityFunctionPtr clamp(DensityFunctionPtr input, float min, float max) {
    return std::make_shared<ClampFunction>(std::move(input), min, max);
}

DensityFunctionPtr min(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::MIN, std::move(left), std::move(right));
}

DensityFunctionPtr max(DensityFunctionPtr left, DensityFunctionPtr right) {
    return std::make_shared<BinaryFunction>(BinaryFunction::Type::MAX, std::move(left), std::move(right));
}

DensityFunctionPtr add(DensityFunctionPtr left, float right) {
    return add(std::move(left), constant(right));
}

DensityFunctionPtr sub(DensityFunctionPtr left, float right) {
    return sub(std::move(left), constant(right));
}

DensityFunctionPtr mul(DensityFunctionPtr left, float right) {
    return mul(std::move(left), constant(right));
}

DensityFunctionPtr div(DensityFunctionPtr left, float right) {
    return div(std::move(left), constant(right));
}

DensityFunctionPtr pow(DensityFunctionPtr base, float exponent) {
    if (exponent == 0.5f) {
        return sqrt(std::move(base));
    } else if (exponent == 2.0f) {
        return square(std::move(base));
    } else {
        return exponent == 3.0f ? cube(std::move(base)) : pow(std::move(base), constant(exponent));
    }
}

DensityFunctionPtr sliceY(DensityFunctionPtr input, int y) {
    return slice(Axis::Y, y, std::move(input));
}

DensityFunctionPtr slice(Axis axis, int coordinate, DensityFunctionPtr input) {
    return std::make_shared<SliceFunction>(axis, coordinate, std::move(input));
}

DensityFunctionPtr yClampedGradient(int fromY, int toY, float fromValue, float toValue) {
    return gradient(Axis::Y, TilingMode::CLAMP_TO_EDGE, fromY, toY, fromValue, toValue);
}

DensityFunctionPtr gradient(Axis axis, TilingMode tiling, int fromCoordinate, int toCoordinate, float fromValue,
                            float toValue) {
    return std::make_shared<GradientFunction>(axis, tiling, fromCoordinate, toCoordinate, fromValue, toValue);
}

DensityFunctionPtr blendAlpha() {
    return SimpleDensityFunction::blendAlpha();
}

DensityFunctionPtr blendOffset() {
    return SimpleDensityFunction::blendOffset();
}

DensityFunctionPtr beardifier() {
    return SimpleDensityFunction::beardifier();
}

DensityFunctionPtr round(RoundFunction::Type type, DensityFunctionPtr input, DensityFunctionPtr multiple) {
    return std::make_shared<RoundFunction>(type, std::move(input), std::move(multiple));
}

DensityFunctionPtr round(RoundFunction::Type type, DensityFunctionPtr input) {
    return round(type, std::move(input), constant(1.0f));
}

DensityFunctionPtr floor(DensityFunctionPtr input, DensityFunctionPtr multiple) {
    return round(RoundFunction::Type::FLOOR, std::move(input), std::move(multiple));
}

DensityFunctionPtr floor(DensityFunctionPtr input) {
    return floor(std::move(input), constant(1.0f));
}

DensityFunctionPtr round(DensityFunctionPtr input, DensityFunctionPtr multiple) {
    return round(RoundFunction::Type::ROUND, std::move(input), std::move(multiple));
}

DensityFunctionPtr round(DensityFunctionPtr input) {
    return round(std::move(input), constant(1.0f));
}

DensityFunctionPtr ceil(DensityFunctionPtr input, DensityFunctionPtr multiple) {
    return round(RoundFunction::Type::CEIL, std::move(input), std::move(multiple));
}

DensityFunctionPtr ceil(DensityFunctionPtr input) {
    return ceil(std::move(input), constant(1.0f));
}

DensityFunctionPtr truncate(DensityFunctionPtr input, DensityFunctionPtr multiple) {
    return round(RoundFunction::Type::TRUNCATE, std::move(input), std::move(multiple));
}

DensityFunctionPtr truncate(DensityFunctionPtr input) {
    return truncate(std::move(input), constant(1.0f));
}

DensityFunctionPtr remap(DensityFunctionPtr input, float fromMin, float fromMax, float toMin, float toMax) {
    const float factor = (toMax - toMin) / (fromMax - fromMin);
    const float offset = toMin - fromMin * factor;
    return offset == 0.0f ? mul(input, factor) : add(mul(input, factor), offset);
}

DensityFunctionPtr clampedMap(DensityFunctionPtr input, float fromMin, float fromMax, float toMin, float toMax) {
    return remap(clamp(std::move(input), fromMin, fromMax), fromMin, fromMax, toMin, toMax);
}

} // namespace DensityFunctions
} // namespace density
} // namespace levelgen
} // namespace minecraft
