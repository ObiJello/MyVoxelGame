/**
 * Quick test to verify LegacyRandomSource nextLong() matches Java
 */

#include <iostream>
#include <iomanip>
#include "random/LegacyRandomSource.h"

int main() {
    // Test with seed 12345 (same as our carver test)
    minecraft::LegacyRandomSource rng(12345);

    std::cout << "=== C++ LegacyRandomSource Test ===" << std::endl;
    std::cout << "Seed: 12345" << std::endl;
    std::cout << std::endl;

    // First nextLong() - this should be xScale
    int64_t xScale = rng.nextLong();
    std::cout << "First nextLong() (xScale): " << xScale << std::endl;

    // Second nextLong() - this should be zScale
    int64_t zScale = rng.nextLong();
    std::cout << "Second nextLong() (zScale): " << zScale << std::endl;

    // Test with fresh random for nextFloat
    minecraft::LegacyRandomSource rng2(12345);
    rng2.nextLong(); // consume xScale
    rng2.nextLong(); // consume zScale

    // What finalSeed would be for chunk (-8, -6)
    int64_t chunkX = -8;
    int64_t chunkZ = -6;
    int64_t seed = 12345;

    minecraft::LegacyRandomSource seedCalc(seed);
    int64_t xs = seedCalc.nextLong();
    int64_t zs = seedCalc.nextLong();
    int64_t finalSeed = chunkX * xs ^ chunkZ * zs ^ seed;

    std::cout << std::endl;
    std::cout << "For chunk (" << chunkX << ", " << chunkZ << "):" << std::endl;
    std::cout << "  xScale: " << xs << std::endl;
    std::cout << "  zScale: " << zs << std::endl;
    std::cout << "  finalSeed: " << finalSeed << std::endl;

    // Now get the first float after setLargeFeatureSeed
    minecraft::LegacyRandomSource finalRng(0);
    finalRng.setLargeFeatureSeed(seed, chunkX, chunkZ);
    float firstFloat = finalRng.nextFloat();
    std::cout << "  First nextFloat after setLargeFeatureSeed: " << std::fixed << std::setprecision(10) << firstFloat << std::endl;

    return 0;
}
