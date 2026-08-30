/**
 * C++ trace of ORE_TUFF placement positions for parity comparison with Java.
 * Build via CMake: make ore_feature_trace
 */

#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include "levelgen/WorldgenRandom.h"
#include "math/Mth.h"

using namespace minecraft;
using namespace minecraft::levelgen;

struct SimpleBlockPos {
    int32_t x, y, z;
    SimpleBlockPos(int32_t x_, int32_t y_, int32_t z_) : x(x_), y(y_), z(z_) {}
};

int main() {
    std::cout << "=== C++ ORE_TUFF Placement Trace ===" << std::endl;
    std::cout << std::setprecision(17);

    int64_t worldSeed = 12345L;
    int32_t chunkX = 500;
    int32_t chunkZ = 500;
    int32_t originX = chunkX * 16;  // 8000
    int32_t originZ = chunkZ * 16;  // 8000

    // Create WorldgenRandom
    XoroshiroRandomSource xorSource(0L);
    WorldgenRandom random(std::move(xorSource));

    // Set decoration seed
    int64_t decorationSeed = random.setDecorationSeed(worldSeed, originX, originZ);
    std::cout << "Decoration seed: " << decorationSeed << std::endl;

    // ORE_TUFF is at step 6, index 8
    random.setFeatureSeed(decorationSeed, 8, 6);
    std::cout << "Feature seed set for ORE_TUFF (step=6, idx=8)" << std::endl;

    std::cout << "\n--- Tracing placement modifier chain ---" << std::endl;

    // Simulate lazy stream evaluation like Java
    std::vector<SimpleBlockPos> finalPositions;

    for (int count = 0; count < 2; count++) {
        std::cout << "Processing count iteration " << count << ":" << std::endl;

        // InSquarePlacement
        int dx = random.nextInt(16);
        int dz = random.nextInt(16);
        int posX = originX + dx;
        int posZ = originZ + dz;
        std::cout << "  InSquarePlacement: dx=" << dx << " dz=" << dz
                  << " -> (" << posX << ", ?, " << posZ << ")" << std::endl;

        // HeightRangePlacement.uniform(-64, 0)
        int y = -64 + random.nextInt(65);
        std::cout << "  HeightRangePlacement: nextInt(65) -> y=" << y << std::endl;

        std::cout << "  BiomeFilter: (assumed pass for plains)" << std::endl;

        std::cout << "  FINAL POSITION: BlockPos{x=" << posX << ", y=" << y << ", z=" << posZ << "}" << std::endl;
        finalPositions.push_back(SimpleBlockPos(posX, y, posZ));
    }

    std::cout << "\n--- Simulating OreFeature.place() for each position ---" << std::endl;

    for (size_t i = 0; i < finalPositions.size(); i++) {
        const SimpleBlockPos& pos = finalPositions[i];
        std::cout << "\n=== OreFeature.place() call " << i << " at BlockPos{x="
                  << pos.x << ", y=" << pos.y << ", z=" << pos.z << "} ===" << std::endl;

        int32_t size = 64;

        // OreFeature.place() random consumption:
        float rawDir = random.nextFloat();
        float dir = rawDir * static_cast<float>(M_PI);
        std::cout << "  dir: nextFloat()=" << rawDir << " -> dir=" << dir << std::endl;

        float spreadXY = static_cast<float>(size) / 8.0f;
        int32_t maxRadius = static_cast<int32_t>(std::ceil(
            (static_cast<float>(size) / 16.0f * 2.0f + 1.0f) / 2.0f
        ));

        double x0 = static_cast<double>(pos.x) + std::sin(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double x1 = static_cast<double>(pos.x) - std::sin(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double z0 = static_cast<double>(pos.z) + std::cos(static_cast<double>(dir)) * static_cast<double>(spreadXY);
        double z1 = static_cast<double>(pos.z) - std::cos(static_cast<double>(dir)) * static_cast<double>(spreadXY);

        int32_t y0_raw = random.nextInt(3);
        double y0 = static_cast<double>(pos.y + y0_raw - 2);
        std::cout << "  y0: nextInt(3)=" << y0_raw << " -> y0=" << y0 << std::endl;

        int32_t y1_raw = random.nextInt(3);
        double y1 = static_cast<double>(pos.y + y1_raw - 2);
        std::cout << "  y1: nextInt(3)=" << y1_raw << " -> y1=" << y1 << std::endl;

        std::cout << "  Vein endpoints:" << std::endl;
        std::cout << "    Start: (" << x0 << ", " << y0 << ", " << z0 << ")" << std::endl;
        std::cout << "    End:   (" << x1 << ", " << y1 << ", " << z1 << ")" << std::endl;

        // Log first 3 sphere radii
        std::cout << "  First 3 sphere radii (nextDouble() values):" << std::endl;
        for (int s = 0; s < 3; s++) {
            double rawDouble = random.nextDouble();
            float step = static_cast<float>(s) / static_cast<float>(size);
            double ss = rawDouble * static_cast<double>(size) / 16.0;
            float sinVal = Mth::sin(static_cast<float>(M_PI) * step);
            double r = (static_cast<double>(sinVal + 1.0f) * ss + 1.0) / 2.0;
            std::cout << "    sphere[" << s << "]: nextDouble()=" << rawDouble << " r=" << r << std::endl;
        }

        // Consume remaining spheres
        for (int s = 3; s < size; s++) {
            random.nextDouble();
        }
    }

    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
