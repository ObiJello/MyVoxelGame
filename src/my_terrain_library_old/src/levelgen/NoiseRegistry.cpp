#include "levelgen/NoiseRegistry.h"
#include "synth/NormalNoise.h"
#include "random/XoroshiroRandomSource.h"

namespace minecraft {
namespace levelgen {

NoiseRegistry::NoiseHolder::~NoiseHolder() {
    // Note: noise is managed by DensityFunction::NoiseHolder, not here
    // So we don't delete it
}

NoiseRegistry::~NoiseRegistry() {
    // Clean up holders
    for (auto& pair : m_holders) {
        delete pair.second;
    }
}

NoiseRegistry& NoiseRegistry::instance() {
    static NoiseRegistry registry;
    return registry;
}

void NoiseRegistry::registerNoise(const std::string& name, int firstOctave, const std::vector<double>& amplitudes) {
    m_noises[name] = NoiseParameters(firstOctave, amplitudes);
}

const NoiseRegistry::NoiseParameters& NoiseRegistry::getOrThrow(const std::string& name) const {
    auto it = m_noises.find(name);
    if (it == m_noises.end()) {
        throw std::runtime_error("Noise not found: " + name);
    }
    return it->second;
}

NoiseRegistry::NoiseHolder* NoiseRegistry::getHolder(const std::string& name) {
    // Check if holder already exists
    auto it = m_holders.find(name);
    if (it != m_holders.end()) {
        return it->second;
    }

    // Create new holder
    const NoiseParameters& params = getOrThrow(name);
    NoiseHolder* holder = new NoiseHolder(name, params);
    m_holders[name] = holder;
    return holder;
}

NormalNoise* NoiseRegistry::instantiate(const std::string& name, XoroshiroRandomSource* randomSource) {
    const NoiseParameters& params = getOrThrow(name);
    return new NormalNoise(NormalNoise::create(*randomSource, params.firstOctave, params.amplitudes));
}

// Bootstrap all noise parameters from NoiseData.java
void NoiseRegistry::bootstrap() {
    NoiseRegistry& registry = instance();

    // Helper lambda for registration
    auto reg = [&](const std::string& name, int firstOctave, const std::vector<double>& amps) {
        registry.registerNoise(name, firstOctave, amps);
    };

    // registerBiomeNoises (octaveOffset=0): TEMPERATURE, VEGETATION, CONTINENTALNESS, EROSION
    // Line 70: temperature: -10, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0}
    reg("temperature", -10, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0});
    // Line 71: vegetation: -8, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0}
    reg("vegetation", -8, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    // Line 72: continentalness: -9, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0}
    reg("continentalness", -9, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0});
    // Line 73: erosion: -9, {1.0, 1.0, 0.0, 1.0, 1.0}
    reg("erosion", -9, {1.0, 1.0, 0.0, 1.0, 1.0});

    // registerBiomeNoises (octaveOffset=-2): TEMPERATURE_LARGE, VEGETATION_LARGE, CONTINENTALNESS_LARGE, EROSION_LARGE
    reg("temperature_large", -12, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0});
    reg("vegetation_large", -10, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    reg("continentalness_large", -11, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0});
    reg("erosion_large", -11, {1.0, 1.0, 0.0, 1.0, 1.0});

    // Line 15: RIDGE
    reg("ridge", -7, {1.0, 2.0, 1.0, 0.0, 0.0, 0.0});

    // Line 16: SHIFT (DEFAULT_SHIFT from line 10)
    reg("offset", -3, {1.0, 1.0, 1.0, 0.0});

    // Aquifer noises (lines 17-20)
    reg("aquifer_barrier", -3, {1.0});
    reg("aquifer_fluid_level_floodedness", -7, {1.0});
    reg("aquifer_lava", -1, {1.0});
    reg("aquifer_fluid_level_spread", -5, {1.0});

    // Pillar noises (lines 21-23)
    reg("pillar", -7, {1.0, 1.0});
    reg("pillar_rareness", -8, {1.0});
    reg("pillar_thickness", -8, {1.0});

    // Spaghetti 2D noises (lines 24-27)
    reg("spaghetti_2d", -7, {1.0});
    reg("spaghetti_2d_elevation", -8, {1.0});
    reg("spaghetti_2d_modulator", -11, {1.0});
    reg("spaghetti_2d_thickness", -11, {1.0});

    // Spaghetti 3D noises (lines 28-31)
    reg("spaghetti_3d_1", -7, {1.0});
    reg("spaghetti_3d_2", -7, {1.0});
    reg("spaghetti_3d_rarity", -11, {1.0});
    reg("spaghetti_3d_thickness", -8, {1.0});

    // Spaghetti roughness (lines 32-33)
    reg("spaghetti_roughness", -5, {1.0});
    reg("spaghetti_roughness_modulator", -8, {1.0});

    // Cave noises (lines 34-36)
    reg("cave_entrance", -7, {0.4, 0.5, 1.0});
    reg("cave_layer", -8, {1.0});
    reg("cave_cheese", -8, {0.5, 1.0, 2.0, 1.0, 2.0, 1.0, 0.0, 2.0, 0.0});

    // Ore vein noises (lines 37-40)
    reg("ore_veininess", -8, {1.0});
    reg("ore_vein_a", -7, {1.0});
    reg("ore_vein_b", -7, {1.0});
    reg("ore_gap", -5, {1.0});

    // Noodle noises (lines 41-44)
    reg("noodle", -8, {1.0});
    reg("noodle_thickness", -8, {1.0});
    reg("noodle_ridge_a", -7, {1.0});
    reg("noodle_ridge_b", -7, {1.0});

    // Line 45: JAGGED (16 octaves!)
    reg("jagged", -16, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});

    // Surface noises (lines 46-48)
    reg("surface", -6, {1.0, 1.0, 1.0});
    reg("surface_secondary", -6, {1.0, 1.0, 0.0, 1.0});
    reg("clay_bands_offset", -8, {1.0});

    // Badlands noises (lines 49-51)
    reg("badlands_pillar", -2, {1.0, 1.0, 1.0, 1.0});
    reg("badlands_pillar_roof", -8, {1.0});
    reg("badlands_surface", -6, {1.0, 1.0, 1.0});

    // Iceberg noises (lines 52-54)
    reg("iceberg_pillar", -6, {1.0, 1.0, 1.0, 1.0});
    reg("iceberg_pillar_roof", -3, {1.0});
    reg("iceberg_surface", -6, {1.0, 1.0, 1.0});

    // Line 55: SWAMP
    reg("surface_swamp", -2, {1.0});

    // Decoration noises (lines 56-60)
    reg("calcite", -9, {1.0, 1.0, 1.0, 1.0});
    reg("gravel", -8, {1.0, 1.0, 1.0, 1.0});
    reg("powder_snow", -6, {1.0, 1.0, 1.0, 1.0});
    reg("packed_ice", -7, {1.0, 1.0, 1.0, 1.0});
    reg("ice", -4, {1.0, 1.0, 1.0, 1.0});

    // Nether layer noises (lines 61-62)
    reg("soul_sand_layer", -8, {1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334});
    reg("gravel_layer", -8, {1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334});

    // Patch and nether noises (lines 63-66)
    reg("patch", -5, {1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334});
    reg("netherrack", -3, {1.0, 0.0, 0.0, 0.35});
    reg("nether_wart", -3, {1.0, 0.0, 0.0, 0.9});
    reg("nether_state_selector", -4, {1.0});
}

} // namespace levelgen
} // namespace minecraft
