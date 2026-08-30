#include <iostream>
#include <iomanip>
#include "levelgen/WorldgenRandom.h"
#include "random/XoroshiroRandomSource.h"
#include "random/RandomSupport.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "core/BlockPos.h"

using namespace minecraft;
using namespace minecraft::levelgen;
using namespace minecraft::levelgen::placement;
using namespace minecraft::levelgen::carver;
using namespace minecraft::core;

int main() {
    std::cout << "=== ORE_GRANITE_LOWER Placement Trace ===" << std::endl << std::endl;
    
    int64_t worldSeed = 12345L;
    int32_t chunkX = 500;
    int32_t chunkZ = 500;
    int32_t originX = chunkX * 16;
    int32_t originZ = chunkZ * 16;
    int32_t minY = -64;  // overworld min
    
    std::cout << "World seed: " << worldSeed << std::endl;
    std::cout << "Chunk: (" << chunkX << ", " << chunkZ << ")" << std::endl;
    std::cout << "Origin: (" << originX << ", " << minY << ", " << originZ << ")" << std::endl;
    
    // Create random like ChunkGenerator does
    XoroshiroRandomSource randomSource{RandomSupport::generateUniqueSeed()};
    WorldgenRandom random{randomSource};
    
    // Set decoration seed
    int64_t decorationSeed = random.setDecorationSeed(worldSeed, originX, originZ);
    std::cout << "Decoration seed: " << decorationSeed << std::endl;
    
    // ORE_GRANITE_LOWER is at step 6, index 3 (based on our output)
    // Let's trace indices 2, 3, 4 to see granite upper/lower
    for (int idx = 2; idx <= 4; idx++) {
        std::cout << "\n=== Step=6 Idx=" << idx << " ===" << std::endl;
        random.setFeatureSeed(decorationSeed, idx, 6);
        std::cout << "Feature seed = " << (decorationSeed + idx + 60000L) << std::endl;
        
        // ORE_GRANITE_LOWER uses commonOrePlacement(2, uniformHeight(0, 60))
        // Modifiers: CountPlacement(2), InSquarePlacement, HeightRangePlacement, BiomeFilter
        
        // 1. CountPlacement(2) - returns 2 copies of origin, no random calls
        std::cout << "CountPlacement(2): returns 2 copies of origin" << std::endl;
        
        // 2. InSquarePlacement applied to each position
        BlockPos origin(originX, minY, originZ);
        std::vector<BlockPos> positions;
        
        for (int p = 0; p < 2; p++) {
            int32_t dx = random.nextInt(16);
            int32_t dz = random.nextInt(16);
            std::cout << "InSquarePlacement[" << p << "]: nextInt(16)=" << dx << ", nextInt(16)=" << dz << std::endl;
            positions.push_back(BlockPos(originX + dx, minY, originZ + dz));
        }
        
        // 3. HeightRangePlacement with uniform(0, 60)
        std::cout << "HeightRangePlacement uniform(0, 60):" << std::endl;
        std::vector<BlockPos> heightPositions;
        for (int p = 0; p < 2; p++) {
            // UniformHeight(0, 60): nextInt(61) + 0
            int32_t y = random.nextInt(61) + 0;
            std::cout << "  [" << p << "]: nextInt(61)=" << y << " -> Y=" << y << std::endl;
            heightPositions.push_back(BlockPos(positions[p].getX(), y, positions[p].getZ()));
        }
        
        // 4. BiomeFilter - no random calls
        std::cout << "BiomeFilter: no random calls" << std::endl;
        
        // 5. OreFeature.place() for each position
        std::cout << "OreFeature.place() for each position:" << std::endl;
        for (int p = 0; p < 2; p++) {
            std::cout << "  Position[" << p << "] = (" << heightPositions[p].getX() 
                      << ", " << heightPositions[p].getY() 
                      << ", " << heightPositions[p].getZ() << ")" << std::endl;
            
            float dir = random.nextFloat();
            int y0_delta = random.nextInt(3) - 2;
            int y1_delta = random.nextInt(3) - 2;
            std::cout << "    nextFloat()=" << std::fixed << std::setprecision(8) << dir << std::endl;
            std::cout << "    nextInt(3)=" << (y0_delta + 2) << " -> y0_delta=" << y0_delta << std::endl;
            std::cout << "    nextInt(3)=" << (y1_delta + 2) << " -> y1_delta=" << y1_delta << std::endl;
            
            // Then 64 nextDouble() calls for sphere data (size=64)
            std::cout << "    64x nextDouble() for sphere data" << std::endl;
            for (int i = 0; i < 64; i++) {
                double d = random.nextDouble();
                if (i < 3) {
                    std::cout << "      [" << i << "]: " << d << std::endl;
                }
            }
            std::cout << "      ..." << std::endl;
        }
    }
    
    return 0;
}
