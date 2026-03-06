#include "levelgen/NoiseRouter.h"

namespace minecraft {
namespace levelgen {

using density::DensityFunction;

NoiseRouter::NoiseRouter(
    DensityFunction* barrierNoise,
    DensityFunction* fluidLevelFloodednessNoise,
    DensityFunction* fluidLevelSpreadNoise,
    DensityFunction* lavaNoise,
    DensityFunction* temperature,
    DensityFunction* vegetation,
    DensityFunction* continents,
    DensityFunction* erosion,
    DensityFunction* depth,
    DensityFunction* ridges,
    DensityFunction* preliminarySurfaceLevel,
    DensityFunction* finalDensity,
    DensityFunction* veinToggle,
    DensityFunction* veinRidged,
    DensityFunction* veinGap
)
    : m_barrierNoise(barrierNoise)
    , m_fluidLevelFloodednessNoise(fluidLevelFloodednessNoise)
    , m_fluidLevelSpreadNoise(fluidLevelSpreadNoise)
    , m_lavaNoise(lavaNoise)
    , m_temperature(temperature)
    , m_vegetation(vegetation)
    , m_continents(continents)
    , m_erosion(erosion)
    , m_depth(depth)
    , m_ridges(ridges)
    , m_preliminarySurfaceLevel(preliminarySurfaceLevel)
    , m_finalDensity(finalDensity)
    , m_veinToggle(veinToggle)
    , m_veinRidged(veinRidged)
    , m_veinGap(veinGap)
{
}

NoiseRouter NoiseRouter::mapAll(DensityFunction::Visitor& visitor) const {
    return NoiseRouter(
        m_barrierNoise->mapAll(visitor),
        m_fluidLevelFloodednessNoise->mapAll(visitor),
        m_fluidLevelSpreadNoise->mapAll(visitor),
        m_lavaNoise->mapAll(visitor),
        m_temperature->mapAll(visitor),
        m_vegetation->mapAll(visitor),
        m_continents->mapAll(visitor),
        m_erosion->mapAll(visitor),
        m_depth->mapAll(visitor),
        m_ridges->mapAll(visitor),
        m_preliminarySurfaceLevel->mapAll(visitor),
        m_finalDensity->mapAll(visitor),
        m_veinToggle->mapAll(visitor),
        m_veinRidged->mapAll(visitor),
        m_veinGap->mapAll(visitor)
    );
}

} // namespace levelgen
} // namespace minecraft
