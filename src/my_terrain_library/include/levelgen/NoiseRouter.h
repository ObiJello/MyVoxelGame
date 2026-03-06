#pragma once

#include "levelgen/DensityFunction.h"

namespace minecraft {
namespace levelgen {

// Use the DensityFunction from minecraft::density namespace
using density::DensityFunction;

class NoiseRouter {
public:
    // Default constructor - all functions are null
    NoiseRouter()
        : m_barrierNoise(nullptr), m_fluidLevelFloodednessNoise(nullptr)
        , m_fluidLevelSpreadNoise(nullptr), m_lavaNoise(nullptr)
        , m_temperature(nullptr), m_vegetation(nullptr)
        , m_continents(nullptr), m_erosion(nullptr)
        , m_depth(nullptr), m_ridges(nullptr)
        , m_preliminarySurfaceLevel(nullptr), m_finalDensity(nullptr)
        , m_veinToggle(nullptr), m_veinRidged(nullptr), m_veinGap(nullptr)
    {}

    // Constructor with all 15 density functions
    NoiseRouter(
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
    );

    // Destructor - does NOT own the density functions
    ~NoiseRouter() = default;

    // Getters for all 15 fields
    DensityFunction* barrierNoise() const { return m_barrierNoise; }
    DensityFunction* fluidLevelFloodednessNoise() const { return m_fluidLevelFloodednessNoise; }
    DensityFunction* fluidLevelSpreadNoise() const { return m_fluidLevelSpreadNoise; }
    DensityFunction* lavaNoise() const { return m_lavaNoise; }
    DensityFunction* temperature() const { return m_temperature; }
    DensityFunction* vegetation() const { return m_vegetation; }
    DensityFunction* continents() const { return m_continents; }
    DensityFunction* erosion() const { return m_erosion; }
    DensityFunction* depth() const { return m_depth; }
    DensityFunction* ridges() const { return m_ridges; }
    DensityFunction* preliminarySurfaceLevel() const { return m_preliminarySurfaceLevel; }
    DensityFunction* finalDensity() const { return m_finalDensity; }
    DensityFunction* veinToggle() const { return m_veinToggle; }
    DensityFunction* veinRidged() const { return m_veinRidged; }
    DensityFunction* veinGap() const { return m_veinGap; }

    // Apply visitor to all density functions (Java: mapAll)
    // Returns a new NoiseRouter with transformed density functions
    NoiseRouter mapAll(DensityFunction::Visitor& visitor) const;

private:
    DensityFunction* m_barrierNoise;
    DensityFunction* m_fluidLevelFloodednessNoise;
    DensityFunction* m_fluidLevelSpreadNoise;
    DensityFunction* m_lavaNoise;
    DensityFunction* m_temperature;
    DensityFunction* m_vegetation;
    DensityFunction* m_continents;
    DensityFunction* m_erosion;
    DensityFunction* m_depth;
    DensityFunction* m_ridges;
    DensityFunction* m_preliminarySurfaceLevel;
    DensityFunction* m_finalDensity;
    DensityFunction* m_veinToggle;
    DensityFunction* m_veinRidged;
    DensityFunction* m_veinGap;
};

} // namespace levelgen
} // namespace minecraft
