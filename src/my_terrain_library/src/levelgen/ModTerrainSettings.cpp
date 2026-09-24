#include "levelgen/ModTerrainSettings.h"

#include "levelgen/TwilightDensityFunctions.h"
#include "levelgen/density/DensityFunctions.h"
#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/synth/BlendedNoise.h"
#include "levelgen/density/synth/NormalNoise.h"
#include "world/level/block/Blocks.h"

#include <mutex>
#include <stdexcept>

namespace minecraft {
namespace levelgen {

namespace df = density::DensityFunctions;
using density::DensityFunctionPtr;
using density::TerrainSettings;
using density::WorldgenRegistries;

namespace {

// NoiseRouterData.slide (26.3; the mods' copies are the same function).
DensityFunctionPtr slide(DensityFunctionPtr caves, int minY, int height, int topStartY, int topEndY, float topTarget,
                         int bottomStartY, int bottomEndY, float bottomTarget) {
    DensityFunctionPtr noiseValue = std::move(caves);
    DensityFunctionPtr topFactor = df::yClampedGradient(minY + height - topStartY, minY + height - topEndY, 1.0f, 0.0f);
    noiseValue = df::lerp(topFactor, topTarget, noiseValue);
    DensityFunctionPtr bottomFactor = df::yClampedGradient(minY + bottomStartY, minY + bottomEndY, 0.0f, 1.0f);
    noiseValue = df::lerp(bottomFactor, bottomTarget, noiseValue);
    return noiseValue;
}

// The pre-26.3 preliminary surface level as a 26.3 function: the highest
// cell-aligned y from the top of the noise range down whose
// initial_density_without_jaggedness exceeds NOISE_ZERO (0.390625), found by
// find_top_surface, then spread over the chunk like the Overworld's
// chunk_surface_level (NoiseRouterData.registerTerrainNoises).
DensityFunctionPtr chunkSurfaceLevel(DensityFunctionPtr initialDensity, int minY, int height, int cellHeight) {
    DensityFunctionPtr density = df::add(std::move(initialDensity), df::constant(-0.390625f));
    DensityFunctionPtr surface =
        df::findTopSurface(density, df::constant(static_cast<float>(minY + height)), minY, cellHeight);
    return df::interpolated(surface, 16, 1);
}

TerrainSettings::BlockState* blockOrThrow(const char* id) {
    TerrainSettings::BlockState* state = world::level::block::Blocks::getDefaultState(id);
    if (state == nullptr) throw std::runtime_error(std::string(id) + " missing from the block registry");
    return state;
}

} // namespace

std::shared_ptr<const TerrainSettings> ModTerrainSettings::aether() {
    WorldgenRegistries& registries = WorldgenRegistries::get();
    // aether:temperature / aether:vegetation (data/aether/worldgen/noise):
    // pre-26.3 {firstOctave, amplitudes} noises, which 26.3 reads through
    // NormalNoise.createParity.
    static std::once_flag noisesOnce;
    std::call_once(noisesOnce, [&registries] {
        registries.registerNoise("aether:temperature",
                                 density::synth::NormalNoise::createParity(-8, {1.5, 0.0, 1.0, 0.0, 0.0, 0.0}));
        registries.registerNoise("aether:vegetation",
                                 density::synth::NormalNoise::createParity(-7, {1.0, 1.0, 0.0, 0.0, 0.0, 0.0}));
        // aether:base_3d_noise_aether: old_blended_noise(0.25, 0.25, 80, 160, 8).
        registries.registerDensityFunction("aether:base_3d_noise_aether",
                                           std::make_shared<density::synth::BlendedNoise>(0.25, 0.25, 80.0, 160.0, 8.0));
    });

    // skylands.json "noise": min_y 0, height 128, size_horizontal 2,
    // size_vertical 1 -> cells 8 x 4.
    constexpr int minY = 0;
    constexpr int height = 128;

    // AetherNoiseBuilders.buildFinalDensity: no x0.64 unlike vanilla postProcess.
    DensityFunctionPtr density = registries.densityFunction("aether:base_3d_noise_aether");
    density = df::add(density, df::constant(-0.13f));
    density = slide(density, minY, height, 72, 0, -0.2f, 8, 40, -0.1f);
    density = df::add(density, df::constant(-0.05f));
    density = df::blendDensity(density);
    density = df::interpolated(density, 8, 4);
    density = df::squeeze(density);

    auto settings = std::make_shared<TerrainSettings>();
    settings->noiseSettings = density::NoiseSettings::create(minY, height);
    settings->defaultBlock = blockOrThrow("minecraft:holystone");
    settings->defaultFluid = blockOrThrow("minecraft:water");
    DensityFunctionPtr shiftX = registries.densityFunction("minecraft:shift_x");
    DensityFunctionPtr shiftZ = registries.densityFunction("minecraft:shift_z");
    density::NoiseRouter& router = settings->noiseRouter;
    router.temperature = df::shiftedNoise2d(shiftX, shiftZ, 0.25, registries.noise("aether:temperature"));
    router.vegetation = df::shiftedNoise2d(shiftX, shiftZ, 0.25, registries.noise("aether:vegetation"));
    router.continents = df::zero();
    router.erosion = df::zero();
    router.depth = df::zero();
    router.ridges = df::zero();
    // The mod's initial_density_without_jaggedness is zero: no surface level.
    router.chunkSurfaceLevel = df::zero();
    router.finalDensity = df::add(density, df::beardifier());
    settings->materialRule = "aether:aether";
    settings->seaLevel = -64;
    settings->disableMobGeneration = false;
    settings->useLegacyRandomSource = false;
    return settings;
}

std::shared_ptr<const TerrainSettings> ModTerrainSettings::twilight(
    std::shared_ptr<const world::biome::TwilightBiomeLayout> layout) {
    WorldgenRegistries& registries = WorldgenRegistries::get();
    // twilight_noise_gen.json "noise": min_y -32, height 256,
    // size_horizontal 2, size_vertical 2 -> cells 8 x 8.
    constexpr int minY = -32;
    constexpr int height = 256;

    // raw_biome_terrain.json: biome_driven_terrain, lower -31, upper 64,
    // depth_scalar 1, base_factor 8, base_offset -1.25.
    DensityFunctionPtr rawBiomeTerrain = std::make_shared<TwilightBiomeDrivenTerrain>(
        layout, -31.0, 64.0, 1.0, df::constant(8.0f), df::constant(-1.25f));
    // raw_biome_noise.json: biome_driven_noise, lower -31, upper 64, depth_scalar 1.
    DensityFunctionPtr rawBiomeNoise = std::make_shared<TwilightBiomeDrivenNoise>(layout, -31.0, 64.0, 1.0);

    density::NoiseHolder ridgeNoise = registries.noise("minecraft:ridge");
    density::NoiseHolder surfaceNoise = registries.noise("minecraft:surface");

    // cache_once(clamp(0.5 + 0.5 * surface(xz 1, y 0), 0, 1)).
    DensityFunctionPtr surfaceMask = df::cache(df::clamp(
        df::add(df::constant(0.5f), df::mul(df::constant(0.5f), df::noise(surfaceNoise, 1.0, 0.0))), 0.0f, 1.0f));
    DensityFunctionPtr ridgeWide = df::add(df::constant(0.5f), df::mul(df::constant(0.5f), df::noise(ridgeNoise, 1.0, 0.0)));
    DensityFunctionPtr ridgeTight = df::add(df::constant(0.5f), df::mul(df::constant(0.5f), df::noise(ridgeNoise, 4.0, 0.0)));
    DensityFunctionPtr ridgeBlend =
        df::add(df::mul(ridgeWide, df::add(df::constant(1.0f), df::mul(df::constant(-1.0f), surfaceMask))),
                df::mul(ridgeTight, surfaceMask));
    // interpolated(max(0, flat_cache(ridgeBlend))).
    DensityFunctionPtr hills =
        df::mul(rawBiomeNoise, df::interpolated(df::max(df::constant(0.0f), df::cache(ridgeBlend)), 8, 8));
    // 0.1666666716337204 is (double)(1F / 6F), as the datagen wrote it.
    DensityFunctionPtr baseTerrain = df::mul(df::constant(1.0f / 6.0f),
                                             df::add(rawBiomeTerrain, df::yClampedGradient(-31, 256, 31.0f, -256.0f)));
    DensityFunctionPtr forestedTerrain = df::clamp(df::add(baseTerrain, hills), -0.1f, 0.5f);

    auto settings = std::make_shared<TerrainSettings>();
    settings->noiseSettings = density::NoiseSettings::create(minY, height);
    settings->defaultBlock = blockOrThrow("minecraft:stone");
    settings->defaultFluid = blockOrThrow("minecraft:water");
    density::NoiseRouter& router = settings->noiseRouter;
    router.temperature = df::zero();
    router.vegetation = df::zero();
    router.continents = df::zero();
    router.erosion = df::zero();
    router.depth = df::zero();
    router.ridges = df::zero();
    // initial_density_without_jaggedness = forested_terrain.
    router.chunkSurfaceLevel = chunkSurfaceLevel(forestedTerrain, minY, height, 8);
    router.finalDensity = df::add(forestedTerrain, df::beardifier());
    settings->materialRule = "twilight_forest:twilight_forest";
    settings->seaLevel = 0;
    settings->disableMobGeneration = false;
    settings->useLegacyRandomSource = false;
    return settings;
}

std::shared_ptr<const TerrainSettings> ModTerrainSettings::hush() {
    WorldgenRegistries& registries = WorldgenRegistries::get();
    std::shared_ptr<const TerrainSettings> overworld = TerrainSettings::load("minecraft:overworld", registries);
    auto settings = std::make_shared<TerrainSettings>(*overworld);
    settings->defaultBlock = blockOrThrow("minecraft:hushstone");
    settings->materialRule = "obeycraft:hush";
    settings->spawnTarget.clear();
    settings->seaLevel = 50;
    settings->debugFunctions.clear();
    return settings;
}

} // namespace levelgen
} // namespace minecraft
