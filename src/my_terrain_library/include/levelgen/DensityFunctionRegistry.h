#pragma once

#include "levelgen/DensityFunction.h"
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace minecraft {
namespace levelgen {

// Forward declaration
class NoiseRegistry;

/**
 * DensityFunctionRegistry - Registry for pre-built DensityFunctions
 *
 * Stores and manages density functions used in terrain generation.
 * Corresponds to Java's Registries.DENSITY_FUNCTION with bootstrap from NoiseRouterData.java
 *
 * Reference: net/minecraft/world/level/levelgen/NoiseRouterData.java lines 73-102
 */
class DensityFunctionRegistry {
public:
    using DensityFunction = minecraft::density::DensityFunction;

    // Singleton access
    static DensityFunctionRegistry& instance();

    // Register a density function
    void registerFunction(const std::string& name, DensityFunction* function);

    // Get a function (throws if not found)
    DensityFunction* getOrThrow(const std::string& name) const;

    // Check if a function exists
    bool has(const std::string& name) const;

    // Get the world seed used for bootstrap
    int64_t getWorldSeed() const { return m_worldSeed; }

    // Bootstrap - populate with all Minecraft density functions
    // Reference: NoiseRouterData.java bootstrap() method
    // Requires NoiseRegistry to be bootstrapped first
    static void bootstrap(int64_t worldSeed);

    // Clear all functions (for cleanup)
    void clear();

    // Helper functions for bootstrap (made public for registerTerrainNoises)
    static DensityFunction* splineWithBlending(DensityFunction* splineFunc, DensityFunction* blendingTarget);
    static DensityFunction* offsetToDepth(DensityFunction* offset);
    static DensityFunction* noiseGradientDensity(DensityFunction* factor, DensityFunction* depthWithJaggedness);

    // Helper to create NoiseHolder from noise name and world seed
    static DensityFunction::NoiseHolder* createNoiseHolder(const std::string& noiseName, int64_t worldSeed);

private:
    DensityFunctionRegistry() : m_worldSeed(0) {}
    ~DensityFunctionRegistry();

    std::unordered_map<std::string, DensityFunction*> m_functions;
    int64_t m_worldSeed;
};

} // namespace levelgen
} // namespace minecraft
