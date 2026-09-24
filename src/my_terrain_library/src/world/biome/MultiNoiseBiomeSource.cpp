#include "world/biome/MultiNoiseBiomeSource.h"
#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityVolume.h"
#include "levelgen/TerrainProvider.h"
#include "levelgen/WorldGenTweaks.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "world/biome/Biomes.h"
#include "world/biome/Biome.h"
#include "core/QuartPos.h"

// Reference: net/minecraft/world/level/biome/MultiNoiseBiomeSource.java

namespace minecraft {
namespace world {
namespace biome {

// Constructor from parameter list
MultiNoiseBiomeSource::MultiNoiseBiomeSource(
    const std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>& parameters
) : m_preset(Preset::OVERWORLD) {
    m_parameters = std::make_unique<Climate::ParameterList<BiomeKey>>(parameters);
    collectBiomes(parameters);
}

// Constructor from preset
MultiNoiseBiomeSource::MultiNoiseBiomeSource(Preset preset)
    : m_preset(preset) {

    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> params;

    switch (preset) {
        case Preset::OVERWORLD:
            params = buildOverworldParameters();
            m_overworldBuilder = std::make_unique<OverworldBiomeBuilder>();
            break;
        case Preset::NETHER:
            params = buildNetherParameters();
            break;
        case Preset::HUSH:
            params = buildHushParameters();
            break;
        case Preset::AETHER:
            params = buildAetherParameters();
            break;
    }

    // World Properties: disabled-biome checklist (non-vanilla). Removing an
    // entry hands its climate space to the nearest remaining biome, exactly
    // like a datapack that drops parameter points. Ignored if it would
    // remove every entry.
    if (preset == Preset::OVERWORLD) {
        const auto& disabled = levelgen::WorldGenTweaks::get().disabledBiomes;
        if (!disabled.empty()) {
            std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> kept;
            kept.reserve(params.size());
            for (const auto& entry : params) {
                if (disabled.find(entry.second) == disabled.end()) {
                    kept.push_back(entry);
                }
            }
            if (!kept.empty()) params = std::move(kept);
        }
    }

    m_parameters = std::make_unique<Climate::ParameterList<BiomeKey>>(params);
    collectBiomes(params);
}

// Factory for overworld
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createOverworld() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::OVERWORLD);
}

// Factory for nether
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createNether() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::NETHER);
}

// Factory for The Hush (engine-only)
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createHush() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::HUSH);
}

// Factory for The Aether (dimension/the_aether.json)
std::unique_ptr<MultiNoiseBiomeSource> MultiNoiseBiomeSource::createAether() {
    return std::make_unique<MultiNoiseBiomeSource>(Preset::AETHER);
}

// Main biome selection method
BiomeKey MultiNoiseBiomeSource::getNoiseBiome(
    int32_t quartX, int32_t quartY, int32_t quartZ,
    const Climate::Sampler& sampler
) {
    // Reference: MultiNoiseBiomeSource.java lines 27-30
    // return this.parameters.findValue(sampler.sample(quartX, quartY, quartZ));
    // The Hush (non-vanilla) reads temperature and humidity at a finer scale.
    Climate::TargetPoint target = m_preset == Preset::HUSH
        ? sampleHushClimate(sampler, quartX, quartY, quartZ)
        : sampler.sample(quartX, quartY, quartZ);
    return m_parameters->findValue(target);
}

BiomeSource::BiomeResolver MultiNoiseBiomeSource::createResolverForChunk(
    const Climate::Sampler& sampler, int32_t minQuartX, int32_t minQuartY, int32_t minQuartZ,
    int32_t quartSizeX, int32_t quartSizeY, int32_t quartSizeZ) {
    if (m_preset == Preset::HUSH) {
        return BiomeSource::createResolverForChunk(sampler, minQuartX, minQuartY, minQuartZ, quartSizeX,
                                                   quartSizeY, quartSizeZ);
    }
    using levelgen::density::DensityBuffer;
    using levelgen::density::DensityVolume;
    const DensityVolume volume(quartSizeX, quartSizeY, quartSizeZ, core::QuartPos::toBlock(minQuartX),
                               core::QuartPos::toBlock(minQuartY), core::QuartPos::toBlock(minQuartZ), 4, 4, 4);
    // DensityBuffer.createUnpooled x6, sampled in Java's order.
    struct Volumes {
        std::vector<float> temperature, vegetation, continents, erosion, depth, ridges;
    };
    auto volumes = std::make_shared<Volumes>();
    auto fill = [&](const levelgen::density::BoundSampler& bound, std::vector<float>& out) {
        std::unique_ptr<DensityBuffer> buffer = DensityBuffer::createUnpooled(volume.size());
        bound.sampleVolume(*buffer, volume);
        out.resize(static_cast<size_t>(volume.size()));
        for (int i = 0; i < volume.size(); ++i) out[static_cast<size_t>(i)] = buffer->get(i);
    };
    fill(sampler.temperature(), volumes->temperature);
    fill(sampler.humidity(), volumes->vegetation);
    fill(sampler.continentalness(), volumes->continents);
    fill(sampler.erosion(), volumes->erosion);
    fill(sampler.depth(), volumes->depth);
    fill(sampler.weirdness(), volumes->ridges);
    Climate::ParameterList<BiomeKey>* parameters = m_parameters.get();
    return [volumes, volume, parameters, minQuartX, minQuartY, minQuartZ](int32_t quartX, int32_t quartY,
                                                                          int32_t quartZ) {
        const size_t index = static_cast<size_t>(
            volume.indexUnchecked(quartX - minQuartX, quartY - minQuartY, quartZ - minQuartZ));
        return parameters->findValue(Climate::target(volumes->temperature[index], volumes->vegetation[index],
                                                     volumes->continents[index], volumes->erosion[index],
                                                     volumes->depth[index], volumes->ridges[index]));
    };
}

Climate::TargetPoint MultiNoiseBiomeSource::sampleHushClimate(const Climate::Sampler& sampler,
                                                              int32_t quartX, int32_t quartY, int32_t quartZ) {
    const int32_t blockX = core::QuartPos::toBlock(quartX);
    const int32_t blockY = core::QuartPos::toBlock(quartY);
    const int32_t blockZ = core::QuartPos::toBlock(quartZ);
    // floor(v * num / den), exact in integers (64-bit so a far-lands
    // coordinate times the numerator cannot overflow before the divide).
    auto scaled = [](int32_t v, int32_t num, int32_t den) {
        const int64_t p = static_cast<int64_t>(v) * num;
        const int64_t q = p >= 0 ? p / den : -((-p + den - 1) / den);
        return static_cast<int32_t>(q);
    };
    const float temperature = sampler.temperature().sampleValue(
        scaled(blockX, kHushTemperatureScaleNum, kHushTemperatureScaleDen), blockY,
        scaled(blockZ, kHushTemperatureScaleNum, kHushTemperatureScaleDen));
    const float humidity = sampler.humidity().sampleValue(
        scaled(blockX, kHushHumidityScaleNum, kHushHumidityScaleDen), blockY,
        scaled(blockZ, kHushHumidityScaleNum, kHushHumidityScaleDen));
    const float continentalness = sampler.continentalness().sampleValue(blockX, blockY, blockZ);
    const float erosion = sampler.erosion().sampleValue(blockX, blockY, blockZ);
    const float depth = sampler.depth().sampleValue(blockX, blockY, blockZ);
    const float weirdness = sampler.weirdness().sampleValue(blockX, blockY, blockZ);
    return Climate::target(temperature, humidity, continentalness, erosion, depth, weirdness);
}

// Spawn target for world spawn position
std::vector<Climate::ParameterPoint> MultiNoiseBiomeSource::getSpawnTarget() const {
    // Reference: MultiNoiseBiomeSource.java line 36
    // return this.preset.map(preset -> preset.biomeSource().getSpawnTarget()).orElse(List.of());

    if (m_overworldBuilder) {
        return m_overworldBuilder->spawnTarget();
    }
    return {};
}

// Debug info — MultiNoiseBiomeSource.java:91-102 (26.3): one line naming the
// climate bands the feet position falls in, for the F3 screen.
void MultiNoiseBiomeSource::addDebugInfo(
    std::vector<std::string>& info,
    int32_t x, int32_t y, int32_t z,
    const Climate::Sampler& sampler
) const {
    const int32_t quartX = x >> 2;
    const int32_t quartY = y >> 2;
    const int32_t quartZ = z >> 2;
    // The Hush shows the temperature / humidity its biome choice reads.
    const Climate::TargetPoint target = m_preset == Preset::HUSH
        ? sampleHushClimate(sampler, quartX, quartY, quartZ)
        : sampler.sample(quartX, quartY, quartZ);

    const float continentalness = Climate::unquantizeCoord(target.continentalness);
    const float erosion = Climate::unquantizeCoord(target.erosion);
    const float temperature = Climate::unquantizeCoord(target.temperature);
    const float humidity = Climate::unquantizeCoord(target.humidity);
    const float weirdness = Climate::unquantizeCoord(target.weirdness);
    const double peaksAndValleys = static_cast<double>(minecraft::levelgen::TerrainProvider::peaksAndValleys(weirdness));

    // MC constructs a fresh OverworldBiomeBuilder for the bands; the nether
    // source has none, so it borrows the overworld's (as vanilla does).
    const OverworldBiomeBuilder* builder = m_overworldBuilder.get();
    static const OverworldBiomeBuilder fallbackBuilder;
    if (!builder) builder = &fallbackBuilder;

    info.push_back("Biome builder PV: " + OverworldBiomeBuilder::getDebugStringForPeaksAndValleys(peaksAndValleys) +
                   " C: " + builder->getDebugStringForContinentalness(continentalness) +
                   " E: " + builder->getDebugStringForErosion(erosion) +
                   " T: " + builder->getDebugStringForTemperature(temperature) +
                   " H: " + builder->getDebugStringForHumidity(humidity));
}

// Build overworld parameters using OverworldBiomeBuilder
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildOverworldParameters() {
    // Reference: MultiNoiseBiomeSource.Preset.OVERWORLD lines 50-54
    // OverworldBiomeBuilder builder = new OverworldBiomeBuilder();
    // builder.addBiomes(pair -> list.add(pair.mapSecond(biomes::getOrThrow)));

    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;

    OverworldBiomeBuilder builder;
    builder.addBiomes([&result](const std::pair<Climate::ParameterPoint, BiomeKey>& pair) {
        result.push_back(pair);
    });

    return result;
}

// Build nether parameters
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildNetherParameters() {
    // Reference: MultiNoiseBiomeSourceParameterList.Preset.NETHER (26.1) -
    // EXACT vanilla point parameters (temperature, humidity, cont, erosion,
    // depth, weirdness, offset), verified against the decompiled source:
    //   (0,    0,   0,0,0,0, 0)     -> nether_wastes
    //   (0,   -0.5, 0,0,0,0, 0)     -> soul_sand_valley
    //   (0.4,  0,   0,0,0,0, 0)     -> crimson_forest
    //   (0,    0.5, 0,0,0,0, 0.375) -> warped_forest
    //   (-0.5, 0,   0,0,0,0, 0.175) -> basalt_deltas
    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;
    result.push_back({Climate::parameters(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                      BiomeKeys::NETHER_WASTES});
    result.push_back({Climate::parameters(0.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                      BiomeKeys::SOUL_SAND_VALLEY});
    result.push_back({Climate::parameters(0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                      BiomeKeys::CRIMSON_FOREST});
    result.push_back({Climate::parameters(0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.375f),
                      BiomeKeys::WARPED_FOREST});
    result.push_back({Climate::parameters(-0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.175f),
                      BiomeKeys::BASALT_DELTAS});
    return result;
}

// Build The Hush parameters (engine-only dimension, no vanilla reference),
// sampled over the vanilla OVERWORLD router, so every axis is live.
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildHushParameters() {
    // A PARTITION of the climate space, the way OverworldBiomeBuilder lays
    // out the Overworld — not one exact point per biome.
    //
    // It used to be single points (meadows at 0,0,0,0,0,0) next to a
    // Crystal Caverns entry spanning every axis but depth. Climate picks the
    // entry with the smallest summed squared distance, and a full-range entry
    // is distance 0 on every axis it spans: at the surface (depth ~0) the
    // caverns cost only (0.2 - depth)^2 = 0.04, while a surface point cost the
    // squared offset on EVERY axis. Anywhere the climate was not almost
    // exactly average, the surface came out as Crystal Caverns — sculk-loam
    // plains with no trees, grass or blooms, because the caverns decorate
    // only underground.
    //
    // Surface biomes now own ranges, each registered at depth 0 and depth
    // 1.0 (OverworldBiomeBuilder.addSurfaceBiome), and the caverns keep the
    // cave band depth 0.2..0.9 (addUndergroundBiome). Continentalness below
    // -0.19 is vanilla's ocean band — here the Sunken Choir, flooded by the
    // Hush's sea level 50. Inland: the frozen temperature band is the Aurora
    // Steppe, the high-|weirdness| bands (peaks and valleys) the Hollow
    // Deep, and the middle splits by humidity (forest wet, barrens hot and
    // dry, meadows between).
    //
    // SIZES (2026-09-22). Measured offline (the terrain library's Overworld
    // router + this table, surface depth, 8192 x 8192 blocks sampled every
    // 16, seeds 1, 42, 12345, 987654321, -7777, 31337; "run" = straight-line
    // walk inside one biome, length-weighted = the stretch you are typically
    // in; "patch" = connected area as an equivalent diameter):
    //   before — vanilla-scale climate, aurora T < -0.45, hollow |W| > 0.55,
    //   barrens T > 0.2 & H < -0.35:
    //     meadows 33 %, sunken choir 28 %, forest 18 %, aurora 12 % (0-24 %,
    //     absent in seed 42), hollow deep 7 %, barrens 1.4 % (0.2-3.3 %);
    //     land walk 800 blocks (575-1053) between biome changes, aurora
    //     patches ~3 km (area-weighted median).
    //   after — temperature 3x and humidity 1.5x the xz frequency
    //   (sampleHushClimate), aurora T < -0.35, hollow |W| > 0.5, barrens
    //   T > 0 & H < -0.1, forest H > 0.1, meadows the rest:
    //     sunken choir 28 % (20-33), meadows 22 % (19-25), forest 17 %
    //     (16-19), barrens 12 % (11-13), aurora 11.5 % (9-16), hollow deep
    //     9 % (7-11); land walk 420 blocks (372-507); mean patch diameter
    //     250-550 blocks for the climate biomes, hollow deep ~150 (its
    //     weirdness band is a strip by nature).
    //   then (same day) — the aurora steppe was the one climate biome still
    //   ~2x the others (a typical walk inside it 800 blocks, mean patch 556):
    //   it took every weirdness, so nothing cut it. The hollow deep now owns
    //   the peaks-and-valleys band at every temperature (the chasms follow
    //   the rugged band there too) and the aurora the middle band only, with
    //   its cutoff at T < -0.3 to keep its share:
    //     sunken choir 28 % (20-33), meadows 21 % (18-23), forest 17 %
    //     (15-18), barrens 12 % (11-13), aurora 11.6 % (8.5-16), hollow deep
    //     11 % (9-13); aurora walk 800 -> 520 blocks, mean patch 556 -> 405;
    //     land walk 366 blocks (335-407). The sunken choir stays the largest
    //     (its runs ~1 km): it is the ocean band, and oceans connect.
    // The sunken choir is the Overworld ocean band, i.e. the terrain the Hush
    // sea level floods (98-100 % of c < -0.19 has its surface under y 50), so
    // it keeps -0.19: moving it would put dry-land biomes on the sea floor.
    // Hollow deep stays on the peaks-and-valleys weirdness band its chasms
    // come from. Changing any threshold re-rolls the biome layout of NEW
    // chunks only (saved chunks keep their biomes).
    //
    // First appearances must stay meadows, forest, barrens, caverns, sunken
    // choir, hollow deep, aurora steppe = BiomeFeatureRegistry::
    // getHushBiomeKeys() (feature-seed order).
    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;
    auto span = [](float min, float max) { return Climate::Parameter::span(min, max); };
    const Climate::Parameter full     = span(-1.0f, 1.0f);
    const Climate::Parameter ocean    = span(-1.2f, -0.19f);
    const Climate::Parameter inland   = span(-0.19f, 1.0f);
    const Climate::Parameter frozen   = span(-1.0f, -0.3f);
    const Climate::Parameter mild     = span(-0.3f, 1.0f);
    const Climate::Parameter midWeird = span(-0.5f, 0.5f);
    const Climate::Parameter dry      = span(-1.0f, -0.1f);

    auto surface = [&](const Climate::Parameter& t, const Climate::Parameter& h,
                       const Climate::Parameter& c, const Climate::Parameter& w,
                       const BiomeKey& biome) {
        for (float depth : {0.0f, 1.0f}) {
            result.push_back({Climate::parameters(t, h, c, full, Climate::Parameter::point(depth), w, 0.0f),
                              biome});
        }
    };

    surface(mild, span(-0.1f, 0.1f), inland, midWeird, BiomeKeys::HUSH_MEADOWS);
    surface(mild, span(0.1f, 1.0f),  inland, midWeird, BiomeKeys::WHISPERWOOD_FOREST);
    surface(span(0.0f, 1.0f), dry,   inland, midWeird, BiomeKeys::RESONANT_BARRENS);
    result.push_back({Climate::parameters(full, full, full, full, span(0.2f, 0.9f), full, 0.0f),
                      BiomeKeys::CRYSTAL_CAVERNS});
    surface(full, full, ocean, full, BiomeKeys::SUNKEN_CHOIR);
    surface(full, full, inland, span(0.5f, 1.0f),   BiomeKeys::HOLLOW_DEEP);
    surface(full, full, inland, span(-1.0f, -0.5f), BiomeKeys::HOLLOW_DEEP);
    surface(frozen, full, inland, midWeird, BiomeKeys::AURORA_STEPPE);
    // The cool, dry corner of the middle band is meadow too.
    surface(span(-0.3f, 0.0f), dry, inland, midWeird, BiomeKeys::HUSH_MEADOWS);
    return result;
}

// The Aether — data/aether/dimension/the_aether.json biome_source
// (minecraft:multi_noise, AetherDimensions / AetherBiomes.buildBiomeSource).
// Every point spans the full range on continentalness, erosion, depth and
// weirdness with offset 0; only temperature x humidity select the biome,
// sampled from the aether:temperature / aether:vegetation shifted noises of
// ModTerrainSettings::aether(). The 14 points are copied in file order.
// ORDER IS LOAD-BEARING: first appearances (meadow, forest, grove, woodland)
// must equal BiomeFeatureRegistry::getAetherBiomeKeys().
std::vector<std::pair<Climate::ParameterPoint, BiomeKey>>
MultiNoiseBiomeSource::buildAetherParameters() {
    std::vector<std::pair<Climate::ParameterPoint, BiomeKey>> result;
    const Climate::Parameter fullRange = Climate::Parameter::span(-1.0f, 1.0f);
    auto span = [](float min, float max) { return Climate::Parameter::span(min, max); };
    auto add = [&](const Climate::Parameter& temperature, const Climate::Parameter& humidity,
                   const BiomeKey& biome) {
        result.push_back({Climate::parameters(temperature, humidity, fullRange, fullRange,
                                               fullRange, fullRange, 0.0f),
                          biome});
    };
    add(span(-1.0f, -0.8f), span(-1.0f, 1.0f), BiomeKeys::SKYROOT_MEADOW);
    add(span(-0.8f, 0.0f), span(-1.0f, 0.0f), BiomeKeys::SKYROOT_MEADOW);
    add(span(-0.8f, 0.0f), span(0.0f, 1.0f), BiomeKeys::SKYROOT_FOREST);
    add(span(0.0f, 0.4f), span(-1.0f, 0.0f), BiomeKeys::SKYROOT_GROVE);
    add(span(0.0f, 0.4f), span(0.0f, 0.8f), BiomeKeys::SKYROOT_FOREST);
    add(span(0.0f, 0.4f), span(0.8f, 1.0f), BiomeKeys::SKYROOT_GROVE);
    add(span(0.4f, 0.93f), span(-1.0f, -0.1f), BiomeKeys::SKYROOT_GROVE);
    add(span(0.4f, 0.93f), span(-0.1f, 1.0f), BiomeKeys::SKYROOT_FOREST);
    add(span(0.93f, 0.94f), span(-1.0f, -0.6f), BiomeKeys::SKYROOT_MEADOW);
    add(span(0.93f, 0.94f), span(-0.6f, -0.3f), BiomeKeys::SKYROOT_GROVE);
    add(span(0.93f, 0.94f), span(-0.3f, 1.0f), BiomeKeys::SKYROOT_FOREST);
    add(span(0.94f, 1.0f), span(-1.0f, -0.1f), BiomeKeys::SKYROOT_MEADOW);
    add(span(0.94f, 1.0f), span(-0.1f, 0.8f), BiomeKeys::SKYROOT_WOODLAND);
    add(span(0.93f, 0.94f), span(0.8f, 1.0f), BiomeKeys::SKYROOT_FOREST);
    return result;
}

} // namespace biome
} // namespace world
} // namespace minecraft
