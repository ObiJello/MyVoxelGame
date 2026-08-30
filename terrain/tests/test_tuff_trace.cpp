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
    std::cout << "=== ORE_TUFF Placement Trace (C++) ===" << std::endl << std::endl;

    int64_t worldSeed = 12345L;
    int32_t chunkX = 500;
    int32_t chunkZ = 500;
    int32_t originX = chunkX * 16;
    int32_t originZ = chunkZ * 16;
    int32_t originY = -64;

    std::cout << "Chunk: (" << chunkX << ", " << chunkZ << ")" << std::endl;
    std::cout << "Origin block coords: (" << originX << ", " << originY << ", " << originZ << ")" << std::endl;

    // Create random like ChunkGenerator does
    XoroshiroRandomSource randomSource{RandomSupport::generateUniqueSeed()};
    WorldgenRandom random{randomSource};

    // Set decoration seed
    int64_t decorationSeed = random.setDecorationSeed(worldSeed, originX, originZ);
    std::cout << "Decoration seed: " << decorationSeed << std::endl;

    // ORE_TUFF is at Step 6, Index 8
    int stepIndex = 6;
    int featureIndex = 8;

    // First trace ORE_DIRT (step=6, index=0) - the FIRST feature
    std::cout << "\n=== ORE_DIRT (step=6, index=0) ===" << std::endl;
    random.setFeatureSeed(decorationSeed, 0, 6);
    std::cout << "After setFeatureSeed for ORE_DIRT:" << std::endl;
    // ORE_DIRT uses commonOrePlacement(7, uniform(0, 160))
    std::cout << "CountPlacement(7) - 7 positions" << std::endl;
    for (int p = 0; p < 7; p++) {
        int dx = random.nextInt(16);
        int dz = random.nextInt(16);
        int h = random.nextInt(161); // uniform(0, 160) = nextInt(161)
        std::cout << "  Pos " << p << ": dx=" << dx << " dz=" << dz << " h=" << h << " -> Y=" << h << std::endl;
    }

    // CountPlacement(2) means 2 positions to process
    // Each position goes through: InSquare(2 calls) -> HeightRange(1 call) -> BiomeFilter(0 calls)
    // With lazy stream evaluation, the sequence should be:
    // pos1: inSquare_x, inSquare_z, heightRange
    // pos2: inSquare_x, inSquare_z, heightRange

    std::cout << "\n=== ORE_TUFF (step=6, index=8) ===" << std::endl;
    random.setFeatureSeed(decorationSeed, featureIndex, stepIndex);
    std::cout << "After setFeatureSeed, all random calls in order:" << std::endl;

    // Position 1
    std::cout << "Position 1:" << std::endl;
    int32_t x1 = random.nextInt(16);
    std::cout << "  inSquare_x: nextInt(16) = " << x1 << std::endl;
    int32_t z1 = random.nextInt(16);
    std::cout << "  inSquare_z: nextInt(16) = " << z1 << std::endl;
    int32_t h1 = random.nextInt(65);
    std::cout << "  heightRange: nextInt(65) = " << h1 << " -> Y = " << (h1 - 64) << std::endl;

    // Position 2
    std::cout << "Position 2:" << std::endl;
    int32_t x2 = random.nextInt(16);
    std::cout << "  inSquare_x: nextInt(16) = " << x2 << std::endl;
    int32_t z2 = random.nextInt(16);
    std::cout << "  inSquare_z: nextInt(16) = " << z2 << std::endl;
    int32_t h2 = random.nextInt(65);
    std::cout << "  heightRange: nextInt(65) = " << h2 << " -> Y = " << (h2 - 64) << std::endl;

    std::cout << "\nFinal positions:" << std::endl;
    std::cout << "  Pos 1: (" << (originX + x1) << ", " << (h1 - 64) << ", " << (originZ + z1) << ")" << std::endl;
    std::cout << "  Pos 2: (" << (originX + x2) << ", " << (h2 - 64) << ", " << (originZ + z2) << ")" << std::endl;

    // Now trace OreFeature random calls for position 1
    std::cout << "\n=== OreFeature random calls for Position 1 ===" << std::endl;
    random.setFeatureSeed(decorationSeed, 8, 6);
    // Skip through placement modifiers (6 calls)
    for (int i = 0; i < 6; i++) random.nextInt(16); // Actually: 4 nextInt(16) + 2 nextInt(65)

    // Reset and do it properly
    random.setFeatureSeed(decorationSeed, 8, 6);
    std::cout << "InSquare pos1: " << random.nextInt(16) << ", " << random.nextInt(16) << std::endl;
    std::cout << "HeightRange pos1: " << random.nextInt(65) << std::endl;
    std::cout << "InSquare pos2: " << random.nextInt(16) << ", " << random.nextInt(16) << std::endl;
    std::cout << "HeightRange pos2: " << random.nextInt(65) << std::endl;

    // Now we're at the point where OreFeature.place() is called for pos1
    std::cout << "\nOreFeature.place for pos1:" << std::endl;
    float dir = random.nextFloat();
    std::cout << "  dir (nextFloat): " << dir << std::endl;
    int y0_offset = random.nextInt(3) - 2;
    std::cout << "  y0 offset (nextInt(3)-2): " << y0_offset << std::endl;
    int y1_offset = random.nextInt(3) - 2;
    std::cout << "  y1 offset (nextInt(3)-2): " << y1_offset << std::endl;

    // For size=64, there are 64 nextDouble calls for sphere radii
    std::cout << "  First 5 nextDouble for sphere radii:" << std::endl;
    for (int i = 0; i < 5; i++) {
        std::cout << "    sphere[" << i << "]: " << random.nextDouble() << std::endl;
    }

    return 0;
}
