#pragma once

#include "core/Vec3i.h"
#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/DensityFunction.h"
#include "levelgen/density/DistanceMetric.h"
#include "levelgen/density/TilingMode.h"
#include "levelgen/density/op/RoundFunction.h"

#include <memory>
#include <vector>

// Reference: densityfunction.DensityFunctions (26.3) - the static factories,
// plus DensityFunction's default methods that build on them (x.add(1.0f) is
// add(x, 1.0f) here, x.pow(2.0f) is pow(x, 2.0f)).

namespace minecraft {
namespace levelgen {
namespace density {

class CubicSpline;

namespace DensityFunctions {

inline constexpr float MAX_REASONABLE_NOISE_VALUE = 1000000.0f;

using ::minecraft::levelgen::density::constant;
using ::minecraft::levelgen::density::zero;

DensityFunctionPtr abs(DensityFunctionPtr input);
DensityFunctionPtr square(DensityFunctionPtr input);
DensityFunctionPtr cube(DensityFunctionPtr input);
DensityFunctionPtr sqrt(DensityFunctionPtr input);
DensityFunctionPtr halfNegative(DensityFunctionPtr input);
DensityFunctionPtr quarterNegative(DensityFunctionPtr input);
DensityFunctionPtr reciprocal(DensityFunctionPtr input);
DensityFunctionPtr negate(DensityFunctionPtr input);
DensityFunctionPtr squeeze(DensityFunctionPtr input);
DensityFunctionPtr log(DensityFunctionPtr input);
DensityFunctionPtr sign(DensityFunctionPtr input);

DensityFunctionPtr interpolated(DensityFunctionPtr function, int cellSizeXz, int cellSizeY);
DensityFunctionPtr cache(DensityFunctionPtr function);

DensityFunctionPtr mappedNoise(NoiseHolder noiseData, double xzScale, double yScale, float minTarget, float maxTarget);
DensityFunctionPtr mappedNoise(NoiseHolder noiseData, double yScale, float minTarget, float maxTarget);
DensityFunctionPtr mappedNoise(NoiseHolder noiseData, float minTarget, float maxTarget);
DensityFunctionPtr shiftedNoise2d(DensityFunctionPtr shiftX, DensityFunctionPtr shiftZ, double xzScale,
                                  NoiseHolder noiseData);
DensityFunctionPtr noise(NoiseHolder noiseData);
DensityFunctionPtr noise(NoiseHolder noiseData, double xzScale, double yScale);
DensityFunctionPtr noise(NoiseHolder noiseData, double yScale);

DensityFunctionPtr rangeChoice(DensityFunctionPtr input, float minInclusive, float maxExclusive,
                               DensityFunctionPtr whenInRange, DensityFunctionPtr whenOutOfRange);
DensityFunctionPtr intervalSelect(DensityFunctionPtr input, std::vector<float> thresholds,
                                  std::vector<DensityFunctionPtr> functions);

DensityFunctionPtr shiftA(NoiseHolder noiseData);
DensityFunctionPtr shiftB(NoiseHolder noiseData);
DensityFunctionPtr shift(NoiseHolder noiseData);
DensityFunctionPtr blendDensity(DensityFunctionPtr input);
DensityFunctionPtr endOuterIslands();
DensityFunctionPtr distanceToPoint(const core::Vec3i& point, DistanceMetric metric);

DensityFunctionPtr add(DensityFunctionPtr left, DensityFunctionPtr right);
DensityFunctionPtr sub(DensityFunctionPtr left, DensityFunctionPtr right);
DensityFunctionPtr mul(DensityFunctionPtr left, DensityFunctionPtr right);
DensityFunctionPtr div(DensityFunctionPtr left, DensityFunctionPtr right);
DensityFunctionPtr pow(DensityFunctionPtr base, DensityFunctionPtr exponent);
DensityFunctionPtr clamp(DensityFunctionPtr input, float min, float max);
DensityFunctionPtr min(DensityFunctionPtr left, DensityFunctionPtr right);
DensityFunctionPtr max(DensityFunctionPtr left, DensityFunctionPtr right);

// DensityFunction default methods with a float operand.
DensityFunctionPtr add(DensityFunctionPtr left, float right);
DensityFunctionPtr sub(DensityFunctionPtr left, float right);
DensityFunctionPtr mul(DensityFunctionPtr left, float right);
DensityFunctionPtr div(DensityFunctionPtr left, float right);
// DensityFunction.pow(float): 0.5 -> sqrt, 2 -> square, 3 -> cube.
DensityFunctionPtr pow(DensityFunctionPtr base, float exponent);

DensityFunctionPtr spline(std::shared_ptr<const CubicSpline> spline);

DensityFunctionPtr sliceY(DensityFunctionPtr input, int y);
DensityFunctionPtr slice(Axis axis, int coordinate, DensityFunctionPtr input);
DensityFunctionPtr yClampedGradient(int fromY, int toY, float fromValue, float toValue);
DensityFunctionPtr gradient(Axis axis, TilingMode tiling, int fromCoordinate, int toCoordinate, float fromValue,
                            float toValue);

DensityFunctionPtr blendAlpha();
DensityFunctionPtr blendOffset();
DensityFunctionPtr beardifier();

DensityFunctionPtr lerp(DensityFunctionPtr alpha, DensityFunctionPtr first, DensityFunctionPtr second);
DensityFunctionPtr lerp(DensityFunctionPtr factor, float first, DensityFunctionPtr second);

DensityFunctionPtr round(RoundFunction::Type type, DensityFunctionPtr input, DensityFunctionPtr multiple);
DensityFunctionPtr round(RoundFunction::Type type, DensityFunctionPtr input);
DensityFunctionPtr floor(DensityFunctionPtr input, DensityFunctionPtr multiple);
DensityFunctionPtr floor(DensityFunctionPtr input);
DensityFunctionPtr round(DensityFunctionPtr input, DensityFunctionPtr multiple);
DensityFunctionPtr round(DensityFunctionPtr input);
DensityFunctionPtr ceil(DensityFunctionPtr input, DensityFunctionPtr multiple);
DensityFunctionPtr ceil(DensityFunctionPtr input);
DensityFunctionPtr truncate(DensityFunctionPtr input, DensityFunctionPtr multiple);
DensityFunctionPtr truncate(DensityFunctionPtr input);

DensityFunctionPtr findTopSurface(DensityFunctionPtr density, DensityFunctionPtr upperBound, int lowerBound,
                                  int stepSize);

DensityFunctionPtr remap(DensityFunctionPtr input, float fromMin, float fromMax, float toMin, float toMax);
DensityFunctionPtr clampedMap(DensityFunctionPtr input, float fromMin, float fromMax, float toMin, float toMax);

} // namespace DensityFunctions
} // namespace density
} // namespace levelgen
} // namespace minecraft
