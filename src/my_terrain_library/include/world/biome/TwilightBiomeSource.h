#pragma once

#include "world/biome/BiomeSource.h"
#include "world/biome/Climate.h"
#include "world/biome/Biomes.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Twilight Forest 4.9 (DimensionId::TwilightForest) biome layout.
//
// Reference (mods_reference/twilightforest/src/main/java/twilightforest/):
//   world/components/biomesources/TFBiomeProvider.java      - the BiomeSource
//   world/components/layer/BiomeDensitySource.java          - column lookup +
//                                                             terrain blending
//   world/components/chunkgenerators/TerrainColumn.java     - per-biome column
//   world/components/layer/*.java, layer/vanillalegacy/**   - the layer stack
// Data (src/generated/resources/data/twilightforest/twilight/):
//   biome_terrain_data/biome_grid.json                      - the 20 columns
//   biome_layer_stack/random_forest_biomes.json             - base stack
//   biome_layer_stack/biomes_along_streams.json             - stream overlay
//
// TFBiomeProvider hands out a biome from a legacy (pre-1.18) layer stack — the
// "GenLayer" machinery MC removed in 1.18, kept alive by the mod — rather than
// from climate noise. The stack produces one KEY biome per quart column; that
// key biome's TerrainColumn then (a) picks the biome for each quart Y (the
// underground biomes under every surface biome) and (b) supplies the depth /
// scale / weight constants that the twilightforest:biome_driven_terrain and
// biome_driven_noise density functions blend over a 8.75-quart radius
// (TwilightDensityFunctions.h). Both consumers share one TwilightBiomeLayout.

namespace minecraft {
namespace world {
namespace biome {

namespace twilight {

// The 22 Twilight Forest biomes, numbered in TF possibleBiomes() order:
// BiomeDensitySource.collectPossibleBiomes() streams the TerrainColumns
// sorted by key-biome identifier, each column's biome_layers in ascending
// elevation, and BiomeSource keeps the first appearance of each. That order
// seeds every Twilight feature (FeatureSorter), so
// BiomeFeatureRegistry::getTwilightBiomeKeys() MUST list the same keys in the
// same order.
enum TwilightBiome : uint8_t {
    UNDERGROUND = 0,
    CLEARING,
    DARK_FOREST,
    DARK_FOREST_CENTER,
    DENSE_FOREST,
    DENSE_MUSHROOM_FOREST,
    ENCHANTED_FOREST,
    FINAL_PLATEAU,
    FIRE_SWAMP,
    FIREFLY_FOREST,
    FOREST,
    GLACIER,
    HIGHLANDS_UNDERGROUND,
    HIGHLANDS,
    LAKE,
    MUSHROOM_FOREST,
    OAK_SAVANNAH,
    SNOWY_FOREST,
    SPOOKY_FOREST,
    STREAM,
    SWAMP,
    THORNLANDS,
    BIOME_COUNT
};

/** "twilightforest:<name>" for a TwilightBiome id. */
const BiomeKey& biomeKey(uint8_t id);

/** The 22 keys in possibleBiomes() order (index == TwilightBiome id). */
const std::vector<BiomeKey>& possibleBiomeKeys();

/**
 * One layer of the legacy stack. Java builds each as a LazyArea over a
 * LazyAreaContext(salt): a pixel function plus a small shared cache. Every
 * layer here is a pure function of (world seed, x, z), so the cache cannot
 * change a result; it only stops the fan-out (a zoom reads four parent
 * pixels, a castle/thorns transformer five or nine) from going exponential.
 * The cache is a lock-free direct-mapped table of packed 64-bit entries, safe
 * to share between worldgen threads.
 */
class Layer {
public:
    Layer(int64_t worldSeed, int64_t salt);
    virtual ~Layer();

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    /** LazyArea.getBiome(x, z). */
    uint8_t get(int32_t x, int32_t z) const;

protected:
    /** The layer's applyPixel. */
    virtual uint8_t compute(int32_t x, int32_t z) const = 0;

    /**
     * RandomContext (layer/vanillalegacy/context/RandomContext.java): the
     * layer seed, re-mixed with the pixel position by initRandom.
     */
    struct RandomContext {
        int64_t seed;
        int64_t rval;

        explicit RandomContext(int64_t layerSeed) : seed(layerSeed), rval(0) {}
        void initRandom(int64_t x, int64_t z);
        int32_t nextRandom(int32_t limit);
        uint8_t random(uint8_t a, uint8_t b) { return nextRandom(2) == 0 ? a : b; }
        uint8_t random(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
    };

    RandomContext contextAt(int32_t x, int32_t z) const;

    int64_t m_seed;   // LazyAreaContext.mixSeed(world seed, salt)

private:
    static constexpr uint32_t CACHE_BITS = 14;
    mutable std::unique_ptr<std::atomic<uint64_t>[]> m_cache;
};

/**
 * TerrainColumn (chunkgenerators/TerrainColumn.java) as biome_grid.json
 * configures it: every depth / scale / weight there is a constant density
 * function, so the column is plain numbers.
 */
struct TerrainColumn {
    uint8_t keyBiome;
    double depth;
    double scale;
    double weight;
    // biome_layers (Double2ObjectSortedMap): elevation key in quarts -> biome,
    // ascending. Never empty for the shipped grid.
    std::vector<std::pair<double, uint8_t>> layers;

    /** TerrainColumn.getBiome(quartY): nearest layer key, ties to the lower. */
    uint8_t biomeAt(int32_t quartY) const;
};

/** BiomeDensitySource.DensityData. */
struct DensityData {
    double depth;
    double scale;
};

} // namespace twilight

/**
 * TwilightBiomeLayout — BiomeDensitySource: the built layer stack
 * (biomes_along_streams over random_forest_biomes) plus the TerrainColumn
 * grid. Immutable after construction and thread-safe; shared by the biome
 * source and the terrain density functions.
 */
class TwilightBiomeLayout {
public:
    /** `seed` is WorldUtil.getOverworldSeed(): the world seed. */
    explicit TwilightBiomeLayout(int64_t seed);
    ~TwilightBiomeLayout();

    TwilightBiomeLayout(const TwilightBiomeLayout&) = delete;
    TwilightBiomeLayout& operator=(const TwilightBiomeLayout&) = delete;

    /** genBiomes.getBiome(quartX, quartZ): the column's key biome id. */
    uint8_t keyBiomeAt(int32_t quartX, int32_t quartZ) const;

    /** BiomeDensitySource.getNoiseBiome — the key biome's column at quartY. */
    uint8_t noiseBiomeAt(int32_t quartX, int32_t quartY, int32_t quartZ) const;

    const twilight::TerrainColumn& column(uint8_t keyBiome) const;

    /**
     * BiomeDensitySource.sampleTerrain(blockX, blockZ): the depth and scale
     * of every column within 8.75 quarts, blended with an exponential
     * falloff ("Thanks k.jpg!").
     */
    twilight::DensityData sampleTerrain(int32_t blockX, int32_t blockZ) const;

    /** Process-unique id (never reused), for per-thread memo keys. */
    uint64_t uid() const { return m_uid; }
    int64_t seed() const { return m_seed; }

private:
    int64_t m_seed;
    uint64_t m_uid;
    std::vector<std::unique_ptr<twilight::Layer>> m_layers;   // ownership
    const twilight::Layer* m_root;                            // biomes_along_streams
    std::array<twilight::TerrainColumn, twilight::BIOME_COUNT> m_columns;
    std::array<bool, twilight::BIOME_COUNT> m_hasColumn;
};

/**
 * TwilightBiomeSource — TFBiomeProvider (type twilightforest:twilight_biomes,
 * terrain_data twilightforest:biome_grid). The Climate sampler is ignored, as
 * in Java.
 */
class TwilightBiomeSource : public BiomeSource {
public:
    explicit TwilightBiomeSource(int64_t seed);

    BiomeKey getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ,
                           const Climate::Sampler& sampler) override;

    /** TFBiomeProvider.getMainBiome: the column's key biome. */
    BiomeKey getMainBiome(int32_t quartX, int32_t quartZ) const;

    void addDebugInfo(std::vector<std::string>& info,
                      int32_t x, int32_t y, int32_t z,
                      const Climate::Sampler& sampler) const override;

    const std::shared_ptr<const TwilightBiomeLayout>& layout() const { return m_layout; }

private:
    std::shared_ptr<const TwilightBiomeLayout> m_layout;
};

} // namespace biome
} // namespace world
} // namespace minecraft
