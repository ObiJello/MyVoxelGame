#include "levelgen/WorldGenTweaks.h"
#include "levelgen/placement/PlacedFeature.h"

namespace minecraft {
namespace levelgen {

WorldGenTweaks& WorldGenTweaks::get() {
    static WorldGenTweaks instance;
    return instance;
}

float WorldGenTweaks::currentStepMultiplier() {
    const WorldGenTweaks& tw = get();
    if (tw.featureDensity == 1.0f && tw.oreDensity == 1.0f &&
        tw.vegetationDensity == 1.0f) {
        return 1.0f;
    }
    int step = placement::PlacedFeature::getCurrentStep();
    if (step < 0 || step > 10) return tw.featureDensity;
    return tw.stepMultiplier(step);
}

} // namespace levelgen
} // namespace minecraft
