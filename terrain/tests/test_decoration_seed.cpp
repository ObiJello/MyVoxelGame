#include <iostream>
#include <iomanip>
#include "levelgen/WorldgenRandom.h"
#include "random/XoroshiroRandomSource.h"
#include "random/RandomSupport.h"

using namespace minecraft;
using namespace minecraft::levelgen;

int main() {
    std::cout << "=== C++ Decoration Seed Calculation ===" << std::endl << std::endl;
    
    int64_t worldSeed = 12345L;
    int32_t chunkX = 500;
    int32_t chunkZ = 500;
    
    // Calculate origin like ChunkGenerator does
    int32_t originX = chunkX * 16;  // 8000
    int32_t originZ = chunkZ * 16;  // 8000
    
    std::cout << "World seed: " << worldSeed << std::endl;
    std::cout << "Chunk: (" << chunkX << ", " << chunkZ << ")" << std::endl;
    std::cout << "Origin: (" << originX << ", " << originZ << ")" << std::endl;
    
    // Create random with unique seed first (gets overwritten by setDecorationSeed)
    XoroshiroRandomSource randomSource{RandomSupport::generateUniqueSeed()};
    WorldgenRandom random{randomSource};
    
    // Set decoration seed
    int64_t decorationSeed = random.setDecorationSeed(worldSeed, originX, originZ);
    std::cout << "\nDecoration seed: " << decorationSeed << std::endl;
    
    // Trace feature seed for step 6, index 0
    std::cout << "\n=== Feature Seed for Step 6, Index 0 ===" << std::endl;
    random.setFeatureSeed(decorationSeed, 0, 6);
    std::cout << "After setFeatureSeed(decorationSeed, 0, 6):" << std::endl;
    std::cout << "  nextInt(16) = " << random.nextInt(16) << std::endl;
    std::cout << "  nextInt(16) = " << random.nextInt(16) << std::endl;
    
    // Feature Seed for Step 6, Index 2 (ORE_GRANITE_UPPER)
    std::cout << "\n=== Feature Seed for Step 6, Index 2 (ORE_GRANITE_UPPER) ===" << std::endl;
    random.setFeatureSeed(decorationSeed, 2, 6);
    std::cout << "After setFeatureSeed(decorationSeed, 2, 6):" << std::endl;
    for (int i = 0; i < 5; i++) {
        std::cout << "  nextInt(16) = " << random.nextInt(16) << std::endl;
    }
    
    // Trace setDecorationSeed step by step
    std::cout << "\n=== setDecorationSeed Step-by-Step ===" << std::endl;
    XoroshiroRandomSource randomSource2{42L};
    WorldgenRandom random2{randomSource2};
    
    // Step 1: setSeed(worldSeed)
    random2.setSeed(worldSeed);
    std::cout << "After setSeed(" << worldSeed << "):" << std::endl;
    
    // Step 2: Get xScale = nextLong() | 1L
    int64_t xScale = random2.nextLong() | 1L;
    std::cout << "  xScale = nextLong() | 1 = " << xScale << std::endl;
    
    // Step 3: Get zScale = nextLong() | 1L  
    int64_t zScale = random2.nextLong() | 1L;
    std::cout << "  zScale = nextLong() | 1 = " << zScale << std::endl;
    
    // Step 4: Calculate result
    int64_t result = static_cast<int64_t>(originX) * xScale + static_cast<int64_t>(originZ) * zScale ^ worldSeed;
    std::cout << "  result = " << originX << " * " << xScale << " + " << originZ << " * " << zScale << " ^ " << worldSeed << std::endl;
    std::cout << "  result = " << result << std::endl;
    
    // Step 5: setSeed(result)
    random2.setSeed(result);
    std::cout << "\nAfter setSeed(result):" << std::endl;
    for (int i = 0; i < 5; i++) {
        std::cout << "  nextInt(100) = " << random2.nextInt(100) << std::endl;
    }
    
    return 0;
}
