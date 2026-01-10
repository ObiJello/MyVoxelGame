#pragma once

#include <string>
#include <unordered_map>
#include <stdexcept>
#include <vector>

namespace minecraft {

// Forward declarations
class NormalNoise;
class XoroshiroRandomSource;

namespace levelgen {

/**
 * NoiseRegistry - Registry for NormalNoise.NoiseParameters
 *
 * Stores and manages noise parameters used throughout terrain generation.
 * Corresponds to Java's Registries.NOISE with bootstrap from NoiseData.java
 *
 * Reference: net/minecraft/data/worldgen/NoiseData.java
 */
class NoiseRegistry {
public:
    /**
     * NoiseParameters - Defines octaves and amplitudes for a noise
     * Corresponds to NormalNoise.NoiseParameters in Java
     */
    struct NoiseParameters {
        int firstOctave;
        std::vector<double> amplitudes;

        NoiseParameters() : firstOctave(0) {}  // Default constructor

        NoiseParameters(int firstOctave_, const std::vector<double>& amplitudes_)
            : firstOctave(firstOctave_), amplitudes(amplitudes_) {}
    };

    /**
     * NoiseHolder - Holds both parameters and optionally the instantiated noise
     * Used when passing noise data around
     */
    struct NoiseHolder {
        std::string name;
        NoiseParameters params;
        NormalNoise* noise;  // Can be nullptr until instantiated

        NoiseHolder(const std::string& name_, const NoiseParameters& params_)
            : name(name_), params(params_), noise(nullptr) {}

        ~NoiseHolder();  // Will delete noise if owned
    };

    // Singleton access
    static NoiseRegistry& instance();

    // Register a noise parameter
    void registerNoise(const std::string& name, int firstOctave, const std::vector<double>& amplitudes);

    // Get noise parameters (throws if not found)
    const NoiseParameters& getOrThrow(const std::string& name) const;

    // Get noise holder (creates if needed)
    NoiseHolder* getHolder(const std::string& name);

    // Instantiate a noise with a specific seed context
    NormalNoise* instantiate(const std::string& name, XoroshiroRandomSource* randomSource);

    // Bootstrap - populate with all Minecraft noise parameters
    // Reference: NoiseData.java lines 12-67
    static void bootstrap();

private:
    NoiseRegistry() = default;
    ~NoiseRegistry();

    std::unordered_map<std::string, NoiseParameters> m_noises;
    std::unordered_map<std::string, NoiseHolder*> m_holders;
};

} // namespace levelgen
} // namespace minecraft
