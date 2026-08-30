/**
 * Random Sequence Test - Compare with Java
 */

#include <iostream>
#include <iomanip>
#include "random/XoroshiroRandomSource.h"
#include "math/Mth.h"

static const int64_t SEED = 12345L;

int main() {
    using namespace minecraft;

    // Create same random as surface system
    XoroshiroRandomSource mainRandom(SEED);
    XoroshiroPositionalRandomFactory noiseRandom = mainRandom.forkPositional();

    // Test at position (720, 0, 430) - the column with issues in chunk 45,26
    int32_t blockX = 720;
    int32_t blockZ = 430;

    std::cout << "=== Random Sequence Test ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Position: (" << blockX << ", 0, " << blockZ << ")" << std::endl;
    std::cout << std::endl;

    // Test Mth::getSeed
    int64_t positionalSeed = Mth::getSeed(blockX, 0, blockZ);
    std::cout << "Mth::getSeed(" << blockX << ", 0, " << blockZ << ") = " << positionalSeed << std::endl;

    // Get positional random
    XoroshiroRandomSource random = noiseRandom.at(blockX, 0, blockZ);

    // Test the sequence used in frozenOceanExtension
    int32_t maxSnowDepth = 2 + random.nextInt(4);
    int32_t minSnowHeight = 63 + 18 + random.nextInt(10);
    std::cout << "maxSnowDepth (2 + nextInt(4)) = " << maxSnowDepth << std::endl;
    std::cout << "minSnowHeight (63 + 18 + nextInt(10)) = " << minSnowHeight << std::endl;

    // Print next 10 doubles
    std::cout << std::fixed << std::setprecision(15);
    std::cout << "\nNext 10 doubles:" << std::endl;
    for (int i = 0; i < 10; i++) {
        std::cout << "  random.nextDouble() #" << i << " = " << random.nextDouble() << std::endl;
    }

    return 0;
}
