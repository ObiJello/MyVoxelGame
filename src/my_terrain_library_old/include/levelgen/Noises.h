#pragma once

#include "synth/NormalNoise.h"
#include "random/PositionalRandomFactory.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

// Reference: net/minecraft/world/level/levelgen/Noises.java

namespace minecraft {
namespace levelgen {

/**
 * NoiseParameters - Defines the parameters for creating a NormalNoise instance
 * Reference: NormalNoise.NoiseParameters in Java
 */
struct NoiseParameters {
    int32_t firstOctave;
    std::vector<double> amplitudes;

    NoiseParameters() : firstOctave(0), amplitudes() {}

    NoiseParameters(int32_t first, std::vector<double> amps)
        : firstOctave(first), amplitudes(std::move(amps)) {}

    // Convenience constructor for uniform amplitudes
    NoiseParameters(int32_t first, double amp1)
        : firstOctave(first), amplitudes{amp1} {}

    NoiseParameters(int32_t first, double amp1, double amp2)
        : firstOctave(first), amplitudes{amp1, amp2} {}

    NoiseParameters(int32_t first, double amp1, double amp2, double amp3)
        : firstOctave(first), amplitudes{amp1, amp2, amp3} {}

    NoiseParameters(int32_t first, double amp1, double amp2, double amp3, double amp4)
        : firstOctave(first), amplitudes{amp1, amp2, amp3, amp4} {}

    NoiseParameters(int32_t first, double amp1, double amp2, double amp3, double amp4, double amp5)
        : firstOctave(first), amplitudes{amp1, amp2, amp3, amp4, amp5} {}

    NoiseParameters(int32_t first, double amp1, double amp2, double amp3, double amp4, double amp5, double amp6)
        : firstOctave(first), amplitudes{amp1, amp2, amp3, amp4, amp5, amp6} {}

    NoiseParameters(int32_t first, double amp1, double amp2, double amp3, double amp4, double amp5, double amp6, double amp7)
        : firstOctave(first), amplitudes{amp1, amp2, amp3, amp4, amp5, amp6, amp7} {}
};

/**
 * Noises - Registry of all noise parameter definitions used in terrain generation
 * Reference: net/minecraft/world/level/levelgen/Noises.java
 *
 * All noise keys are defined here with their parameters.
 * These match the Minecraft data pack definitions.
 */
class Noises {
public:
    // Climate noises - Reference: Noises.java lines 11-18
    static const std::string TEMPERATURE;
    static const std::string VEGETATION;
    static const std::string CONTINENTALNESS;
    static const std::string EROSION;
    static const std::string TEMPERATURE_LARGE;
    static const std::string VEGETATION_LARGE;
    static const std::string CONTINENTALNESS_LARGE;
    static const std::string EROSION_LARGE;
    static const std::string RIDGE;

    // Shift/offset noise - Reference: Noises.java line 20
    static const std::string SHIFT;

    // Aquifer noises - Reference: Noises.java lines 21-24
    static const std::string AQUIFER_BARRIER;
    static const std::string AQUIFER_FLUID_LEVEL_FLOODEDNESS;
    static const std::string AQUIFER_LAVA;
    static const std::string AQUIFER_FLUID_LEVEL_SPREAD;

    // Pillar noises - Reference: Noises.java lines 25-27
    static const std::string PILLAR;
    static const std::string PILLAR_RARENESS;
    static const std::string PILLAR_THICKNESS;

    // Spaghetti 2D cave noises - Reference: Noises.java lines 28-31
    static const std::string SPAGHETTI_2D;
    static const std::string SPAGHETTI_2D_ELEVATION;
    static const std::string SPAGHETTI_2D_MODULATOR;
    static const std::string SPAGHETTI_2D_THICKNESS;

    // Spaghetti 3D cave noises - Reference: Noises.java lines 32-36
    static const std::string SPAGHETTI_3D_1;
    static const std::string SPAGHETTI_3D_2;
    static const std::string SPAGHETTI_3D_RARITY;
    static const std::string SPAGHETTI_3D_THICKNESS;
    static const std::string SPAGHETTI_ROUGHNESS;
    static const std::string SPAGHETTI_ROUGHNESS_MODULATOR;

    // Cave noises - Reference: Noises.java lines 38-40
    static const std::string CAVE_ENTRANCE;
    static const std::string CAVE_LAYER;
    static const std::string CAVE_CHEESE;

    // Ore vein noises - Reference: Noises.java lines 41-44
    static const std::string ORE_VEININESS;
    static const std::string ORE_VEIN_A;
    static const std::string ORE_VEIN_B;
    static const std::string ORE_GAP;

    // Noodle cave noises - Reference: Noises.java lines 45-48
    static const std::string NOODLE;
    static const std::string NOODLE_THICKNESS;
    static const std::string NOODLE_RIDGE_A;
    static const std::string NOODLE_RIDGE_B;

    // Surface noises - Reference: Noises.java lines 49-58
    static const std::string JAGGED;
    static const std::string SURFACE;
    static const std::string SURFACE_SECONDARY;
    static const std::string CLAY_BANDS_OFFSET;
    static const std::string BADLANDS_PILLAR;
    static const std::string BADLANDS_PILLAR_ROOF;
    static const std::string BADLANDS_SURFACE;
    static const std::string ICEBERG_PILLAR;
    static const std::string ICEBERG_PILLAR_ROOF;
    static const std::string ICEBERG_SURFACE;

    // Biome-specific noises - Reference: Noises.java lines 59-70
    static const std::string SWAMP;
    static const std::string CALCITE;
    static const std::string GRAVEL;
    static const std::string POWDER_SNOW;
    static const std::string PACKED_ICE;
    static const std::string ICE;
    static const std::string SOUL_SAND_LAYER;
    static const std::string GRAVEL_LAYER;
    static const std::string PATCH;
    static const std::string NETHERRACK;
    static const std::string NETHER_WART;
    static const std::string NETHER_STATE_SELECTOR;

    /**
     * Get noise parameters for a given noise key
     * @param key The noise identifier (e.g., "temperature", "vegetation")
     * @return The NoiseParameters for creating the noise
     */
    static const NoiseParameters& getParameters(const std::string& key);

    /**
     * Check if a noise key exists in the registry
     */
    static bool hasNoise(const std::string& key);

    /**
     * Create a NormalNoise instance for the given key
     * Reference: Noises.java lines 76-79
     *
     * @param key The noise identifier
     * @param randomFactory The positional random factory for seeding
     * @return A new NormalNoise instance
     */
    static NormalNoise* instantiate(
        const std::string& key,
        random::PositionalRandomFactory& randomFactory
    );

    /**
     * Initialize all noise parameters
     * Must be called before using the registry
     */
    static void bootstrap();

private:
    static std::unordered_map<std::string, NoiseParameters> s_noiseParameters;
    static bool s_initialized;

    static void registerNoise(const std::string& key, const NoiseParameters& params);
};

} // namespace levelgen
} // namespace minecraft
