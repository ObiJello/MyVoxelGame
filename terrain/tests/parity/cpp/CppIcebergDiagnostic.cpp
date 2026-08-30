/**
 * Diagnostic to check iceberg noise values at specific positions
 * Compare with Java to find precision differences
 */

#include <iostream>
#include <iomanip>
#include <cmath>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"

static const int64_t SEED = 12345L;

int main(int argc, char* argv[]) {
    // Parse chunk coordinates
    int chunkX = 46;
    int chunkZ = 24;
    if (argc >= 3) {
        chunkX = std::atoi(argv[1]);
        chunkZ = std::atoi(argv[2]);
    }

    std::cout << "=== Iceberg Noise Diagnostic ===" << std::endl;
    std::cout << "Chunk: (" << chunkX << ", " << chunkZ << ")" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << std::endl;

    // Initialize
    minecraft::levelgen::NoiseRegistry::bootstrap();
    minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);
    minecraft::levelgen::SurfaceRuleData::initialize();

    minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);
    minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    minecraft::levelgen::NoiseGeneratorSettings* settings = new minecraft::levelgen::NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    minecraft::levelgen::RandomState* randomState = minecraft::levelgen::RandomState::create(settings, SEED);

    // Get iceberg noises
    minecraft::NormalNoise* icebergSurfaceNoise = randomState->getOrCreateNoise("iceberg_surface");
    minecraft::NormalNoise* icebergPillarNoise = randomState->getOrCreateNoise("iceberg_pillar");
    minecraft::NormalNoise* icebergPillarRoofNoise = randomState->getOrCreateNoise("iceberg_pillar_roof");

    std::cout << std::fixed << std::setprecision(15);

    // Check specific positions within the chunk
    int baseX = chunkX * 16;
    int baseZ = chunkZ * 16;

    std::cout << "Position,icebergSurface,icebergPillar,iceberg,icebergRoof,extensionTop" << std::endl;

    for (int localZ = 0; localZ < 16; localZ++) {
        for (int localX = 0; localX < 4; localX++) {  // Sample every 4 blocks
            int blockX = baseX + localX * 4;
            int blockZ = baseZ + localZ;

            // Calculate iceberg value (same as SurfaceSystem.cpp)
            double pillarScale = 1.28;
            double surfaceVal = icebergSurfaceNoise->getValue(
                static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
            double pillarVal = icebergPillarNoise->getValue(
                static_cast<double>(blockX) * pillarScale,
                0.0,
                static_cast<double>(blockZ) * pillarScale);

            double iceberg = std::min(
                std::abs(surfaceVal * 8.25),
                pillarVal * 15.0
            );

            // Calculate roof
            double roofScale = 1.17;
            double roofAmplitude = 1.5;
            double roofVal = icebergPillarRoofNoise->getValue(
                static_cast<double>(blockX) * roofScale,
                0.0,
                static_cast<double>(blockZ) * roofScale);
            double icebergRoof = std::abs(roofVal * roofAmplitude);

            // Calculate extension top
            double extensionTop = 0.0;
            if (iceberg > 1.8) {
                double top = std::min(
                    iceberg * iceberg * 1.2,
                    std::ceil(icebergRoof * 40.0) + 14.0);
                if (top > 2.0) {
                    extensionTop = 63.0 + top;  // seaLevel + top
                }
            }

            std::cout << "(" << blockX << "," << blockZ << "),"
                      << surfaceVal << ","
                      << pillarVal << ","
                      << iceberg << ","
                      << icebergRoof << ","
                      << extensionTop << std::endl;
        }
    }

    // Clean up
    delete randomState;
    delete settings;

    return 0;
}
