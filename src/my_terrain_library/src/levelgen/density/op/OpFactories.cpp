#include "levelgen/density/DensityFunctions.h"

#include "levelgen/density/CubicSpline.h"
#include "levelgen/density/op/BlendDensityFunction.h"
#include "levelgen/density/op/FindTopSurfaceFunction.h"
#include "levelgen/density/op/InterpolatedFunction.h"
#include "levelgen/density/op/IntervalSelectFunction.h"
#include "levelgen/density/op/LerpFunction.h"
#include "levelgen/density/op/RangeChoiceFunction.h"
#include "levelgen/density/op/SplineFunction.h"

// Reference: densityfunction.DensityFunctions (26.3) - the factories for
// lerp, interpolated, range_choice, interval_select, find_top_surface,
// blend_density and spline.

namespace minecraft {
namespace levelgen {
namespace density {
namespace DensityFunctions {

DensityFunctionPtr lerp(DensityFunctionPtr alpha, DensityFunctionPtr first, DensityFunctionPtr second) {
    return std::make_shared<LerpFunction>(std::move(alpha), std::move(first), std::move(second));
}

DensityFunctionPtr lerp(DensityFunctionPtr factor, float first, DensityFunctionPtr second) {
    return lerp(std::move(factor), constant(first), std::move(second));
}

DensityFunctionPtr interpolated(DensityFunctionPtr function, int cellSizeXz, int cellSizeY) {
    return std::make_shared<InterpolatedFunction>(std::move(function), cellSizeXz, cellSizeY);
}

DensityFunctionPtr rangeChoice(DensityFunctionPtr input, float minInclusive, float maxExclusive,
                               DensityFunctionPtr whenInRange, DensityFunctionPtr whenOutOfRange) {
    return std::make_shared<RangeChoiceFunction>(std::move(input), minInclusive, maxExclusive,
                                                 std::move(whenInRange), std::move(whenOutOfRange));
}

DensityFunctionPtr intervalSelect(DensityFunctionPtr input, std::vector<float> thresholds,
                                  std::vector<DensityFunctionPtr> functions) {
    return std::make_shared<IntervalSelectFunction>(std::move(input), std::move(thresholds), std::move(functions));
}

DensityFunctionPtr findTopSurface(DensityFunctionPtr density, DensityFunctionPtr upperBound, int lowerBound,
                                  int stepSize) {
    return std::make_shared<FindTopSurfaceFunction>(std::move(density), std::move(upperBound), lowerBound, stepSize);
}

DensityFunctionPtr blendDensity(DensityFunctionPtr input) {
    return std::make_shared<BlendDensityFunction>(std::move(input));
}

DensityFunctionPtr spline(std::shared_ptr<const CubicSpline> spline) {
    return std::make_shared<SplineFunction>(std::move(spline));
}

} // namespace DensityFunctions
} // namespace density
} // namespace levelgen
} // namespace minecraft
