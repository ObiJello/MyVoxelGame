#include "levelgen/Noises.h"
#include <stdexcept>

// Reference: net/minecraft/world/level/levelgen/Noises.java
// All noise parameters are loaded from data/minecraft/worldgen/noise/*.json

namespace minecraft {
namespace levelgen {

// Static member initialization
std::unordered_map<std::string, NoiseParameters> Noises::s_noiseParameters;
bool Noises::s_initialized = false;

// Noise key definitions - Reference: Noises.java lines 11-70
const std::string Noises::TEMPERATURE = "temperature";
const std::string Noises::VEGETATION = "vegetation";
const std::string Noises::CONTINENTALNESS = "continentalness";
const std::string Noises::EROSION = "erosion";
const std::string Noises::TEMPERATURE_LARGE = "temperature_large";
const std::string Noises::VEGETATION_LARGE = "vegetation_large";
const std::string Noises::CONTINENTALNESS_LARGE = "continentalness_large";
const std::string Noises::EROSION_LARGE = "erosion_large";
const std::string Noises::RIDGE = "ridge";
const std::string Noises::SHIFT = "offset";  // Note: Java uses "offset" as the key

const std::string Noises::AQUIFER_BARRIER = "aquifer_barrier";
const std::string Noises::AQUIFER_FLUID_LEVEL_FLOODEDNESS = "aquifer_fluid_level_floodedness";
const std::string Noises::AQUIFER_LAVA = "aquifer_lava";
const std::string Noises::AQUIFER_FLUID_LEVEL_SPREAD = "aquifer_fluid_level_spread";

const std::string Noises::PILLAR = "pillar";
const std::string Noises::PILLAR_RARENESS = "pillar_rareness";
const std::string Noises::PILLAR_THICKNESS = "pillar_thickness";

const std::string Noises::SPAGHETTI_2D = "spaghetti_2d";
const std::string Noises::SPAGHETTI_2D_ELEVATION = "spaghetti_2d_elevation";
const std::string Noises::SPAGHETTI_2D_MODULATOR = "spaghetti_2d_modulator";
const std::string Noises::SPAGHETTI_2D_THICKNESS = "spaghetti_2d_thickness";

const std::string Noises::SPAGHETTI_3D_1 = "spaghetti_3d_1";
const std::string Noises::SPAGHETTI_3D_2 = "spaghetti_3d_2";
const std::string Noises::SPAGHETTI_3D_RARITY = "spaghetti_3d_rarity";
const std::string Noises::SPAGHETTI_3D_THICKNESS = "spaghetti_3d_thickness";
const std::string Noises::SPAGHETTI_ROUGHNESS = "spaghetti_roughness";
const std::string Noises::SPAGHETTI_ROUGHNESS_MODULATOR = "spaghetti_roughness_modulator";

const std::string Noises::CAVE_ENTRANCE = "cave_entrance";
const std::string Noises::CAVE_LAYER = "cave_layer";
const std::string Noises::CAVE_CHEESE = "cave_cheese";

const std::string Noises::ORE_VEININESS = "ore_veininess";
const std::string Noises::ORE_VEIN_A = "ore_vein_a";
const std::string Noises::ORE_VEIN_B = "ore_vein_b";
const std::string Noises::ORE_GAP = "ore_gap";

const std::string Noises::NOODLE = "noodle";
const std::string Noises::NOODLE_THICKNESS = "noodle_thickness";
const std::string Noises::NOODLE_RIDGE_A = "noodle_ridge_a";
const std::string Noises::NOODLE_RIDGE_B = "noodle_ridge_b";

const std::string Noises::JAGGED = "jagged";
const std::string Noises::SURFACE = "surface";
const std::string Noises::SURFACE_SECONDARY = "surface_secondary";
const std::string Noises::CLAY_BANDS_OFFSET = "clay_bands_offset";
const std::string Noises::BADLANDS_PILLAR = "badlands_pillar";
const std::string Noises::BADLANDS_PILLAR_ROOF = "badlands_pillar_roof";
const std::string Noises::BADLANDS_SURFACE = "badlands_surface";
const std::string Noises::ICEBERG_PILLAR = "iceberg_pillar";
const std::string Noises::ICEBERG_PILLAR_ROOF = "iceberg_pillar_roof";
const std::string Noises::ICEBERG_SURFACE = "iceberg_surface";

const std::string Noises::SWAMP = "surface_swamp";
const std::string Noises::CALCITE = "calcite";
const std::string Noises::GRAVEL = "gravel";
const std::string Noises::POWDER_SNOW = "powder_snow";
const std::string Noises::PACKED_ICE = "packed_ice";
const std::string Noises::ICE = "ice";
const std::string Noises::SOUL_SAND_LAYER = "soul_sand_layer";
const std::string Noises::GRAVEL_LAYER = "gravel_layer";
const std::string Noises::PATCH = "patch";
const std::string Noises::NETHERRACK = "netherrack";
const std::string Noises::NETHER_WART = "nether_wart";
const std::string Noises::NETHER_STATE_SELECTOR = "nether_state_selector";

void Noises::registerNoise(const std::string& key, const NoiseParameters& params) {
    s_noiseParameters[key] = params;
}

void Noises::bootstrap() {
    if (s_initialized) return;

    // Climate noises - from data/minecraft/worldgen/noise/*.json
    // temperature.json: firstOctave=-10, amplitudes=[1.5, 0.0, 1.0, 0.0, 0.0, 0.0]
    registerNoise(TEMPERATURE, NoiseParameters(-10, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0}));

    // vegetation.json: firstOctave=-8, amplitudes=[1.0, 1.0, 0.0, 0.0, 0.0, 0.0]
    registerNoise(VEGETATION, NoiseParameters(-8, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0}));

    // continentalness.json: firstOctave=-9, amplitudes=[1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0]
    registerNoise(CONTINENTALNESS, NoiseParameters(-9, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0}));

    // erosion.json: firstOctave=-9, amplitudes=[1.0, 1.0, 0.0, 1.0, 1.0]
    registerNoise(EROSION, NoiseParameters(-9, {1.0, 1.0, 0.0, 1.0, 1.0}));

    // Large biome variants
    // temperature_large.json: firstOctave=-12, amplitudes=[1.5, 0.0, 1.0, 0.0, 0.0, 0.0]
    registerNoise(TEMPERATURE_LARGE, NoiseParameters(-12, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0}));

    // vegetation_large.json: firstOctave=-10, amplitudes=[1.0, 1.0, 0.0, 0.0, 0.0, 0.0]
    registerNoise(VEGETATION_LARGE, NoiseParameters(-10, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0}));

    // continentalness_large.json: firstOctave=-11, amplitudes=[1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0]
    registerNoise(CONTINENTALNESS_LARGE, NoiseParameters(-11, {1.0, 1.0, 2.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0}));

    // erosion_large.json: firstOctave=-11, amplitudes=[1.0, 1.0, 0.0, 1.0, 1.0]
    registerNoise(EROSION_LARGE, NoiseParameters(-11, {1.0, 1.0, 0.0, 1.0, 1.0}));

    // ridge.json: firstOctave=-7, amplitudes=[1.0, 2.0, 1.0, 0.0, 0.0, 0.0]
    registerNoise(RIDGE, NoiseParameters(-7, {1.0, 2.0, 1.0, 0.0, 0.0, 0.0}));

    // offset.json (SHIFT): firstOctave=-3, amplitudes=[1.0, 1.0, 1.0, 0.0]
    registerNoise(SHIFT, NoiseParameters(-3, {1.0, 1.0, 1.0, 0.0}));

    // Aquifer noises
    // aquifer_barrier.json: firstOctave=-3, amplitudes=[1.0]
    registerNoise(AQUIFER_BARRIER, NoiseParameters(-3, {1.0}));

    // aquifer_fluid_level_floodedness.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(AQUIFER_FLUID_LEVEL_FLOODEDNESS, NoiseParameters(-7, {1.0}));

    // aquifer_lava.json: firstOctave=-1, amplitudes=[1.0]
    registerNoise(AQUIFER_LAVA, NoiseParameters(-1, {1.0}));

    // aquifer_fluid_level_spread.json: firstOctave=-5, amplitudes=[1.0]
    registerNoise(AQUIFER_FLUID_LEVEL_SPREAD, NoiseParameters(-5, {1.0}));

    // Pillar noises
    // pillar.json: firstOctave=-7, amplitudes=[1.0, 1.0]
    registerNoise(PILLAR, NoiseParameters(-7, {1.0, 1.0}));

    // pillar_rareness.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(PILLAR_RARENESS, NoiseParameters(-8, {1.0}));

    // pillar_thickness.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(PILLAR_THICKNESS, NoiseParameters(-8, {1.0}));

    // Spaghetti 2D cave noises
    // spaghetti_2d.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(SPAGHETTI_2D, NoiseParameters(-7, {1.0}));

    // spaghetti_2d_elevation.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(SPAGHETTI_2D_ELEVATION, NoiseParameters(-8, {1.0}));

    // spaghetti_2d_modulator.json: firstOctave=-11, amplitudes=[1.0]
    registerNoise(SPAGHETTI_2D_MODULATOR, NoiseParameters(-11, {1.0}));

    // spaghetti_2d_thickness.json: firstOctave=-11, amplitudes=[1.0]
    registerNoise(SPAGHETTI_2D_THICKNESS, NoiseParameters(-11, {1.0}));

    // Spaghetti 3D cave noises
    // spaghetti_3d_1.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(SPAGHETTI_3D_1, NoiseParameters(-7, {1.0}));

    // spaghetti_3d_2.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(SPAGHETTI_3D_2, NoiseParameters(-7, {1.0}));

    // spaghetti_3d_rarity.json: firstOctave=-11, amplitudes=[1.0]
    registerNoise(SPAGHETTI_3D_RARITY, NoiseParameters(-11, {1.0}));

    // spaghetti_3d_thickness.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(SPAGHETTI_3D_THICKNESS, NoiseParameters(-8, {1.0}));

    // spaghetti_roughness.json: firstOctave=-5, amplitudes=[1.0]
    registerNoise(SPAGHETTI_ROUGHNESS, NoiseParameters(-5, {1.0}));

    // spaghetti_roughness_modulator.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(SPAGHETTI_ROUGHNESS_MODULATOR, NoiseParameters(-8, {1.0}));

    // Cave noises
    // cave_entrance.json: firstOctave=-7, amplitudes=[0.4, 0.5, 1.0]
    registerNoise(CAVE_ENTRANCE, NoiseParameters(-7, {0.4, 0.5, 1.0}));

    // cave_layer.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(CAVE_LAYER, NoiseParameters(-8, {1.0}));

    // cave_cheese.json: firstOctave=-8, amplitudes=[0.5, 1.0, 2.0, 1.0, 2.0, 1.0, 0.0, 2.0, 0.0]
    registerNoise(CAVE_CHEESE, NoiseParameters(-8, {0.5, 1.0, 2.0, 1.0, 2.0, 1.0, 0.0, 2.0, 0.0}));

    // Ore vein noises
    // ore_veininess.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(ORE_VEININESS, NoiseParameters(-8, {1.0}));

    // ore_vein_a.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(ORE_VEIN_A, NoiseParameters(-7, {1.0}));

    // ore_vein_b.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(ORE_VEIN_B, NoiseParameters(-7, {1.0}));

    // ore_gap.json: firstOctave=-5, amplitudes=[1.0]
    registerNoise(ORE_GAP, NoiseParameters(-5, {1.0}));

    // Noodle cave noises
    // noodle.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(NOODLE, NoiseParameters(-8, {1.0}));

    // noodle_thickness.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(NOODLE_THICKNESS, NoiseParameters(-8, {1.0}));

    // noodle_ridge_a.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(NOODLE_RIDGE_A, NoiseParameters(-7, {1.0}));

    // noodle_ridge_b.json: firstOctave=-7, amplitudes=[1.0]
    registerNoise(NOODLE_RIDGE_B, NoiseParameters(-7, {1.0}));

    // Surface noises
    // jagged.json: firstOctave=-16, amplitudes=[1.0 x 16]
    registerNoise(JAGGED, NoiseParameters(-16, {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}));

    // surface.json: firstOctave=-6, amplitudes=[1.0, 1.0, 1.0]
    registerNoise(SURFACE, NoiseParameters(-6, {1.0, 1.0, 1.0}));

    // surface_secondary.json: firstOctave=-6, amplitudes=[1.0, 1.0, 0.0, 1.0]
    registerNoise(SURFACE_SECONDARY, NoiseParameters(-6, {1.0, 1.0, 0.0, 1.0}));

    // clay_bands_offset.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(CLAY_BANDS_OFFSET, NoiseParameters(-8, {1.0}));

    // badlands_pillar.json: firstOctave=-2, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(BADLANDS_PILLAR, NoiseParameters(-2, {1.0, 1.0, 1.0, 1.0}));

    // badlands_pillar_roof.json: firstOctave=-8, amplitudes=[1.0]
    registerNoise(BADLANDS_PILLAR_ROOF, NoiseParameters(-8, {1.0}));

    // badlands_surface.json: firstOctave=-6, amplitudes=[1.0, 1.0, 1.0]
    registerNoise(BADLANDS_SURFACE, NoiseParameters(-6, {1.0, 1.0, 1.0}));

    // iceberg_pillar.json: firstOctave=-6, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(ICEBERG_PILLAR, NoiseParameters(-6, {1.0, 1.0, 1.0, 1.0}));

    // iceberg_pillar_roof.json: firstOctave=-3, amplitudes=[1.0]
    registerNoise(ICEBERG_PILLAR_ROOF, NoiseParameters(-3, {1.0}));

    // iceberg_surface.json: firstOctave=-6, amplitudes=[1.0, 1.0, 1.0]
    registerNoise(ICEBERG_SURFACE, NoiseParameters(-6, {1.0, 1.0, 1.0}));

    // Biome-specific noises
    // surface_swamp.json: firstOctave=-2, amplitudes=[1.0]
    registerNoise(SWAMP, NoiseParameters(-2, {1.0}));

    // calcite.json: firstOctave=-9, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(CALCITE, NoiseParameters(-9, {1.0, 1.0, 1.0, 1.0}));

    // gravel.json: firstOctave=-8, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(GRAVEL, NoiseParameters(-8, {1.0, 1.0, 1.0, 1.0}));

    // powder_snow.json: firstOctave=-6, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(POWDER_SNOW, NoiseParameters(-6, {1.0, 1.0, 1.0, 1.0}));

    // packed_ice.json: firstOctave=-7, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(PACKED_ICE, NoiseParameters(-7, {1.0, 1.0, 1.0, 1.0}));

    // ice.json: firstOctave=-4, amplitudes=[1.0, 1.0, 1.0, 1.0]
    registerNoise(ICE, NoiseParameters(-4, {1.0, 1.0, 1.0, 1.0}));

    // soul_sand_layer.json: firstOctave=-8, amplitudes=[1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334]
    registerNoise(SOUL_SAND_LAYER, NoiseParameters(-8, {1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334}));

    // gravel_layer.json: firstOctave=-8, amplitudes=[1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334]
    registerNoise(GRAVEL_LAYER, NoiseParameters(-8, {1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334}));

    // patch.json: firstOctave=-5, amplitudes=[1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334]
    registerNoise(PATCH, NoiseParameters(-5, {1.0, 0.0, 0.0, 0.0, 0.0, 0.013333333333333334}));

    // netherrack.json: firstOctave=-3, amplitudes=[1.0, 0.0, 0.0, 0.35]
    registerNoise(NETHERRACK, NoiseParameters(-3, {1.0, 0.0, 0.0, 0.35}));

    // nether_wart.json: firstOctave=-3, amplitudes=[1.0, 0.0, 0.0, 0.9]
    registerNoise(NETHER_WART, NoiseParameters(-3, {1.0, 0.0, 0.0, 0.9}));

    // nether_state_selector.json: firstOctave=-4, amplitudes=[1.0]
    registerNoise(NETHER_STATE_SELECTOR, NoiseParameters(-4, {1.0}));

    s_initialized = true;
}

const NoiseParameters& Noises::getParameters(const std::string& key) {
    if (!s_initialized) {
        bootstrap();
    }

    auto it = s_noiseParameters.find(key);
    if (it == s_noiseParameters.end()) {
        throw std::runtime_error("Unknown noise key: " + key);
    }
    return it->second;
}

bool Noises::hasNoise(const std::string& key) {
    if (!s_initialized) {
        bootstrap();
    }
    return s_noiseParameters.find(key) != s_noiseParameters.end();
}

NormalNoise* Noises::instantiate(
    const std::string& key,
    random::PositionalRandomFactory& randomFactory
) {
    // Reference: Noises.java lines 76-79
    // Holder<NormalNoise.NoiseParameters> holder = noises.getOrThrow(name);
    // return NormalNoise.create(context.fromHashOf(((ResourceKey)holder.unwrapKey().orElseThrow()).identifier()), holder.value());

    const NoiseParameters& params = getParameters(key);

    // Create the positional random from the noise key
    // The key is hashed using "minecraft:" + key format
    std::string fullKey = "minecraft:" + key;
    auto random = randomFactory.fromHashOf(fullKey);

    // Create the NormalNoise with the parameters
    return new NormalNoise(NormalNoise::create(random, params.firstOctave, params.amplitudes));
}

} // namespace levelgen
} // namespace minecraft
