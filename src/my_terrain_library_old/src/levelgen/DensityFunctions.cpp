#include "levelgen/DensityFunctions.h"
#include "synth/SimplexNoise.h"
#include "random/LegacyRandomSource.h"
#include "math/Mth.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace minecraft {
namespace density {
namespace DensityFunctions {

// ============================================================================
// CONSTANT - Static ZERO instance
// ============================================================================

Constant* Constant::s_zero = nullptr;

Constant* Constant::ZERO() {
    if (s_zero == nullptr) {
        s_zero = new Constant(0.0);  // Java line 1218
    }
    return s_zero;
}

// ============================================================================
// TWO-ARGUMENT FUNCTION FACTORY
// ============================================================================

/**
 * Create two-argument function with automatic min/max calculation and optimization
 * Java: DensityFunctions.TwoArgumentSimpleFunction.create (line 913)
 */
DensityFunction* createTwoArgumentFunction(TwoArgType type, DensityFunction* argument1, DensityFunction* argument2) {
    double min1 = argument1->minValue();
    double min2 = argument2->minValue();
    double max1 = argument1->maxValue();
    double max2 = argument2->maxValue();

    // Calculate resulting min and max values based on operation type
    double minValue, maxValue;

    // Calculate minValue (Java lines 926-935)
    switch (type) {
        case TwoArgType::ADD:
            minValue = min1 + min2;
            break;

        case TwoArgType::MUL:
            // Complex logic for multiplication bounds
            if (min1 > 0.0 && min2 > 0.0) {
                minValue = min1 * min2;
            } else if (max1 < 0.0 && max2 < 0.0) {
                minValue = max1 * max2;
            } else {
                minValue = std::min(min1 * max2, max1 * min2);
            }
            break;

        case TwoArgType::MIN:
            minValue = std::min(min1, min2);
            break;

        case TwoArgType::MAX:
            minValue = std::max(min1, min2);
            break;

        default:
            throw std::runtime_error("Unknown TwoArgType in minValue calculation");
    }

    // Calculate maxValue (Java lines 936-944)
    switch (type) {
        case TwoArgType::ADD:
            maxValue = max1 + max2;
            break;

        case TwoArgType::MUL:
            // Complex logic for multiplication bounds
            if (min1 > 0.0 && min2 > 0.0) {
                maxValue = max1 * max2;
            } else if (max1 < 0.0 && max2 < 0.0) {
                maxValue = min1 * min2;
            } else {
                maxValue = std::max(min1 * min2, max1 * max2);
            }
            break;

        case TwoArgType::MIN:
            maxValue = std::min(max1, max2);
            break;

        case TwoArgType::MAX:
            maxValue = std::max(max1, max2);
            break;

        default:
            throw std::runtime_error("Unknown TwoArgType in maxValue calculation");
    }

    // Optimization: if one argument is constant, use MulOrAdd (Java lines 945-955)
    if (type == TwoArgType::MUL || type == TwoArgType::ADD) {
        Constant* constant1 = dynamic_cast<Constant*>(argument1);
        if (constant1 != nullptr) {
            MulOrAdd::SpecificType specificType = (type == TwoArgType::ADD) ?
                MulOrAdd::SpecificType::ADD : MulOrAdd::SpecificType::MUL;
            return new MulOrAdd(specificType, argument2, minValue, maxValue, constant1->value());
        }

        Constant* constant2 = dynamic_cast<Constant*>(argument2);
        if (constant2 != nullptr) {
            MulOrAdd::SpecificType specificType = (type == TwoArgType::ADD) ?
                MulOrAdd::SpecificType::ADD : MulOrAdd::SpecificType::MUL;
            return new MulOrAdd(specificType, argument1, minValue, maxValue, constant2->value());
        }
    }

    // General case: use Ap2 (Java line 957)
    return new Ap2(type, argument1, argument2, minValue, maxValue);
}

} // namespace DensityFunctions

// ============================================================================
// END ISLAND DENSITY FUNCTION (in density namespace, not DensityFunctions)
// ============================================================================

EndIslandDensityFunction::EndIslandDensityFunction(int64_t seed) {
    // Java line 517-521:
    // RandomSource islandRandom = new LegacyRandomSource(seed);
    // islandRandom.consumeCount(17292);
    // this.islandNoise = new SimplexNoise(islandRandom);

    LegacyRandomSource islandRandom(seed);
    islandRandom.consumeCount(17292);
    m_islandNoise = new ::minecraft::synth::SimplexNoise(islandRandom);
}

EndIslandDensityFunction::~EndIslandDensityFunction() {
    delete m_islandNoise;
}

double EndIslandDensityFunction::compute(const DensityFunction::FunctionContext& context) const {
    // Java line 549-551:
    // return ((double)getHeightValue(this.islandNoise, context.blockX() / 8, context.blockZ() / 8) - (double)8.0F) / (double)128.0F
    float height = getHeightValue(m_islandNoise, context.blockX() / 8, context.blockZ() / 8);
    return (static_cast<double>(height) - static_cast<double>(8.0F)) / static_cast<double>(128.0F);
}

float EndIslandDensityFunction::getHeightValue(::minecraft::synth::SimplexNoise* islandNoise, int sectionX, int sectionZ) {
    // Java lines 523-547
    int chunkX = sectionX / 2;
    int chunkZ = sectionZ / 2;
    int subSectionX = sectionX % 2;
    int subSectionZ = sectionZ % 2;

    // Base distance offset - central island falloff
    float doffs = 100.0F - Mth::sqrt(static_cast<float>(sectionX * sectionX + sectionZ * sectionZ)) * 8.0F;
    doffs = Mth::clamp(doffs, -100.0F, 80.0F);

    // Loop through nearby chunks to find outer islands
    for (int xo = -12; xo <= 12; ++xo) {
        for (int zo = -12; zo <= 12; ++zo) {
            int64_t totalChunkX = static_cast<int64_t>(chunkX + xo);
            int64_t totalChunkZ = static_cast<int64_t>(chunkZ + zo);

            // Check if this is an outer island location
            // Must be > 64 chunks from origin and pass noise check
            if (totalChunkX * totalChunkX + totalChunkZ * totalChunkZ > 4096L &&
                islandNoise->getValue(static_cast<double>(totalChunkX), static_cast<double>(totalChunkZ)) < static_cast<double>(-0.9F)) {

                // Calculate island size based on position
                float islandSize = (std::abs(static_cast<float>(totalChunkX)) * 3439.0F +
                                   std::abs(static_cast<float>(totalChunkZ)) * 147.0F);
                islandSize = std::fmod(islandSize, 13.0F) + 9.0F;

                // Calculate distance from island center
                float xd = static_cast<float>(subSectionX - xo * 2);
                float zd = static_cast<float>(subSectionZ - zo * 2);
                // NOTE: Must compute product separately to prevent FMA optimization
                // which would give different results than Java's strict FP semantics
                float distFromCenter = Mth::sqrt(xd * xd + zd * zd);
                float scaled = distFromCenter * islandSize;
                float newDoffs = 100.0F - scaled;
                newDoffs = Mth::clamp(newDoffs, -100.0F, 80.0F);

                // Keep the maximum height
                doffs = std::max(doffs, newDoffs);
            }
        }
    }

    return doffs;
}

} // namespace density
} // namespace minecraft
