// Repro: MyTerrainGenerator-style double Initialize (world -> quit to title ->
// load world again in the SAME process), which re-runs every registry
// bootstrap. Mirrors MyTerrainGenerator::Initialize step order.
#include "levelgen/WorldGenTweaks.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/RandomState.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/ChunkGenerator.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/level/block/Blocks.h"
#include <cstdio>
#include <string>

namespace mc = minecraft;
using mc::world::level::block::Blocks;
namespace levelgen = mc::levelgen;

static void oneWorld(int64_t seed, bool withTweaks) {
    std::printf("== world seed=%lld tweaks=%d ==\n", (long long)seed, withTweaks);
    levelgen::WorldGenTweaks::reset();
    if (withTweaks) {
        auto& tw = levelgen::WorldGenTweaks::get();
        tw.featureDensity = 3.0f;
        tw.disabledBiomes = {"minecraft:plains", "minecraft:forest"};
    }
    Blocks::bootstrap();
    levelgen::NoiseRegistry::bootstrap();
    levelgen::DensityFunctionRegistry::bootstrap(seed);
    levelgen::SurfaceRuleData::initialize();

    auto* router = levelgen::NoiseRouterData::overworld(false, false);
    auto noiseSettings = levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    auto spawnStorage = mc::world::biome::OverworldBiomeBuilder().spawnTarget();
    std::vector<levelgen::ClimateParameterPoint*> spawnPtrs;
    for (auto& p : spawnStorage)
        spawnPtrs.push_back(reinterpret_cast<levelgen::ClimateParameterPoint*>(&p));
    auto* settings = new levelgen::NoiseGeneratorSettings(
        noiseSettings, Blocks::STONE->defaultBlockState(),
        Blocks::WATER->defaultBlockState(), *router, nullptr, spawnPtrs,
        63, false, true, true, false);
    auto* randomState = levelgen::RandomState::create(settings, seed);
    auto* surfaceRules = levelgen::SurfaceRuleData::overworld();
    auto* fluidPicker = new levelgen::OverworldFluidPicker(
        63, -54, Blocks::WATER->defaultBlockState(), Blocks::LAVA->defaultBlockState());
    auto biomeSource = mc::world::biome::MultiNoiseBiomeSource::createOverworld();
    auto* generator = new levelgen::NoiseBasedChunkGenerator(
        settings, randomState->surfaceSystem(), surfaceRules,
        Blocks::STONE->defaultBlockState(), Blocks::AIR->defaultBlockState(),
        fluidPicker, nullptr);
    generator->setBiomeSource(biomeSource.get());
    std::printf("  components created OK\n");

    // Reload-determinism probe: climate samples must depend only on the seed,
    // never on how many worlds this process generated before.
    if (randomState->sampler()) {
        for (int q : {0, 137, -2048}) {
            auto t = randomState->sampler()->sample(q, 0, -q);
            std::printf("  sample(%d): %lld %lld %lld %lld %lld %lld\n", q,
                (long long)t.temperature, (long long)t.humidity,
                (long long)t.continentalness, (long long)t.erosion,
                (long long)t.depth, (long long)t.weirdness);
        }
    }

    // MyTerrainGenerator::Shutdown order.
    delete generator;
    biomeSource.reset();
    delete fluidPicker;
    delete randomState;
    delete settings;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "single") {
        oneWorld(12345, true);      // fresh-process reference
        std::printf("ALL OK\n");
        return 0;
    }
    oneWorld(12345, true);          // create the custom world, play, leave
    oneWorld(-987654321012345678LL, false);  // maybe visit another world
    oneWorld(12345, true);          // load the custom world again
    std::printf("ALL OK\n");
    return 0;
}
