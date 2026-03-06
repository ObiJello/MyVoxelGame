#include "levelgen/DensityFunction.h"
#include "levelgen/Blender.h"
#include "synth/NormalNoise.h"

namespace minecraft {
namespace density {

// Default implementation returns empty blender
Blender* DensityFunction::FunctionContext::getBlender() const {
    return Blender::empty();
}

// NoiseHolder implementations
double DensityFunction::NoiseHolder::getValue(double x, double y, double z) const {
    // If noise is null, return 0.0 (matches Java: (double)0.0F)
    if (m_noise == nullptr) {
        return 0.0;
    }
    return m_noise->getValue(x, y, z);
}

double DensityFunction::NoiseHolder::maxValue() const {
    // If noise is null, return 2.0 (matches Java: (double)2.0F)
    if (m_noise == nullptr) {
        return 2.0;
    }
    return m_noise->maxValue();
}

} // namespace density
} // namespace minecraft
