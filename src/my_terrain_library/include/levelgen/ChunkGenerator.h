#pragma once

#include <mutex>
#include <unordered_map>
#include "levelgen/GenerationStep.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/FeatureSorter.h"
#include "levelgen/Blender.h"
#include "levelgen/placement/PlacementContext.h"
#include "levelgen/carver/CarvingMask.h"
#include "levelgen/Heightmap.h"
#include "world/ChunkPos.h"
#include "world/IChunk.h"
#include "world/biome/Biome.h"
#include "world/biome/BiomeSource.h"
#include "core/BlockPos.h"
#include "core/SectionPos.h"
#include "random/XoroshiroRandomSource.h"
#include "random/RandomSupport.h"
#include <vector>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <cstdint>

// Reference: net/minecraft/world/level/chunk/ChunkGenerator.java

namespace minecraft {

// Forward declarations
namespace levelgen { namespace placement { class PlacedFeature; } }
namespace world {
    class ProtoChunk;
    class LevelChunkSection;
}

namespace levelgen {

// Forward declarations
class RandomState;
class NoiseGeneratorSettings;
namespace density {
class DensitySampler;
class NoiseChunk;
class Aquifer;
class SamplerContext;
}
namespace material {
class MaterialRule;
}

namespace carver {
    class WorldCarverBase;
    class CarvingContext;
}

/**
 * ChunkGenerator - Abstract base for chunk generation
 * Reference: ChunkGenerator.java
 */
class ChunkGenerator {
public:
    virtual ~ChunkGenerator() = default;

    // What 26.3's ChunkGenerator.buildTerrain reads from its WorldGenRegion
    // and StructureManager, gathered by the TERRAIN status task.
    struct TerrainContext {
        int64_t seed = 0;
        // WorldGenRegion.getBiomeManager().getBiome: the fuzzed lookup over
        // the biomes stored in the region's chunks (the material rules).
        std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter;
        // Beardifier.forStructuresInChunk(structureManager, chunk.getPos());
        // null = Beardifier.EMPTY.
        std::shared_ptr<const density::DensitySampler> beardifier;
    };

    /**
     * Reference: ChunkGenerator.buildTerrain (26.3) - the TERRAIN step: the
     * noise fill, the material (surface) rules and the carvers, on one
     * NoiseChunk.
     */
    virtual void buildTerrain(RandomState* randomState, const TerrainContext& context, ::world::IChunk* chunk) = 0;

    /**
     * Apply biome decoration (features) to a chunk
     * Reference: ChunkGenerator.java lines 269-375
     *
     * Uses 3x3 chunk biome collection and global feature indices for proper seeding.
     */
    void applyBiomeDecoration(
        WorldGenLevel* level,
        ::world::IChunk* chunk,
        const std::vector<StepFeatureData>& featuresPerStep
    );

    /**
     * Reference: StructureManager.shouldGenerateStructures(). Set by the
     * harness when it injects the structure state; gates the structure pass
     * inside applyBiomeDecoration.
     */
    void setGenerateStructures(bool enabled) { m_generateStructures = enabled; }
    bool generateStructures() const { return m_generateStructures; }

    //=========================================================================
    // Feature Logging Control (for parity debugging)
    //=========================================================================

    /**
     * Enable/disable feature logging with specified level
     * @param enabled Whether logging is enabled
     * @param level Log detail level: 0=off, 1=step headers, 2=feature names, 3=random state
     */
    static void setFeatureLoggingEnabled(bool enabled, int level = 2);

    /**
     * Set output stream for feature logging (default is std::cerr)
     */
    static void setFeatureLogStream(std::ostream* stream);

    /**
     * Get the sea level
     * Reference: ChunkGenerator.java line 538
     */
    virtual int32_t getSeaLevel() const = 0;

    /**
     * Get minimum Y coordinate
     * Reference: ChunkGenerator.java line 540
     */
    virtual int32_t getMinY() const = 0;

    /**
     * Get generation depth (height)
     * Reference: ChunkGenerator.java line 399
     */
    virtual int32_t getGenDepth() const = 0;

    /**
     * Get base height at the given position
     * Reference: ChunkGenerator.java getBaseHeight()
     *
     * @param x Block X coordinate
     * @param z Block Z coordinate
     * @param heightmapType Type of heightmap to query
     * @param randomState Random state for noise
     * @return Y coordinate of the base height
     */
    virtual int32_t getBaseHeight(
        int32_t x,
        int32_t z,
        Heightmap::Types heightmapType,
        RandomState* randomState
    ) const = 0;

    /**
     * Get a column of blocks at the given position
     * Reference: ChunkGenerator.java getBaseColumn()
     *
     * Fills the provided vector with block types from minY to maxY
     * This is used for structure placement checks
     *
     * @param x Block X coordinate
     * @param z Block Z coordinate
     * @param randomState Random state for noise
     * @param outColumn Output vector to fill with block types (sized to height)
     */
    virtual void getBaseColumn(
        int32_t x,
        int32_t z,
        RandomState* randomState,
        std::vector<BlockState*>& outColumn
    ) const = 0;

    /**
     * Create biomes for a chunk
     * Reference: ChunkGenerator.java createBiomes()
     *
     * @param randomState Random state for noise
     * @param blender Blender for biome blending at chunk edges
     * @param chunk The chunk to populate with biomes
     */
    virtual void createBiomes(
        RandomState* randomState,
        Blender* blender,
        ::world::IChunk* chunk
    ) = 0;

    /**
     * Spawn original mobs in a region
     * Reference: ChunkGenerator.java spawnOriginalMobs()
     * Note: This is typically a no-op for terrain generation
     */
    virtual void spawnOriginalMobs(::world::IChunk* chunk) {
        // Default implementation does nothing
    }

    /**
     * Get biome generation settings for a biome
     * Reference: ChunkGenerator.java getBiomeGenerationSettings()
     *
     * Returns the BiomeGenerationSettings which tracks which features
     * can generate in a specific biome. Used by BiomeFilter.
     *
     * @param biome The biome to get settings for
     * @return BiomeGenerationSettings for the biome
     */
    virtual const world::biome::BiomeGenerationSettings& getBiomeGenerationSettings(
        world::biome::BiomeHolder biome
    ) const {
        // Default implementation returns empty settings (allows all features)
        // Subclasses should override to provide biome-specific feature lists
        return world::biome::BiomeGenerationSettings::empty();
    }

    /**
     * Reference: Java ChunkGenerator's generationSettingsGetter (biome ->
     * BiomeGenerationSettings). The default (nullptr) means "use the global
     * BiomeFeatureRegistry lists"; FlatLevelSource overrides it with its
     * adjusted per-step lists (FlatLevelGeneratorSettings.
     * adjustGenerationSettings). Indexed [step][featureIndex].
     */
    virtual const std::vector<std::vector<const placement::PlacedFeature*>>*
    featuresForBiomeOverride(const std::string& /*biomeKey*/) const {
        return nullptr;
    }

    /**
     * Reference: BiomeFilter.shouldPlace ->
     * context.generator().getBiomeGenerationSettings(biome).hasFeature(f).
     * Default routes to BiomeFeatureRegistry::hasFeature; FlatLevelSource
     * consults its adjusted lists. Implemented in ChunkGenerator.cpp.
     */
    virtual bool hasFeatureInBiome(const std::string& biomeKey,
                                   const placement::PlacedFeature* feature) const;

    /**
     * Reference: Java ChunkGenerator.featuresPerStep - built per GENERATOR
     * from Suppliers.memoize(FeatureSorter.buildFeaturesPerStep(
     * List.copyOf(biomeSource.possibleBiomes()), ...)). The default (nullptr)
     * keeps the historical per-dimension static builds in ChunkStatusTasks
     * (whose key ORDER is parity-proven). Generators
     * whose possibleBiomes differ from a full dimension (FixedBiomeSource
     * single-biome worlds, FlatLevelSource) MUST override, or feature
     * indices for setFeatureSeed diverge from Java.
     */
    virtual const std::vector<StepFeatureData>* customFeaturesPerStep() {
        return nullptr;
    }

    /**
     * Anchor of the columns getBaseColumn() fills: column[i] = block at
     * y = getBaseColumnMinY() + i. Reference: Java NoiseColumn carries its
     * own minY - the noise generator anchors at ITS minY, FlatLevelSource at
     * the LEVEL's minY (heightAccessor.getMinY()). For noise dimensions the
     * two are equal, which is why the old y - getMinY() convention held.
     */
    virtual int32_t getBaseColumnMinY() const { return getMinY(); }

    /**
     * The hosting LEVEL's height range (Java LevelHeightAccessor). Equal to
     * the generator range for every noise dimension; FlatLevelSource
     * overrides with the real level range (its own is 0/384 regardless of
     * the level). Java structure code mixes the two - port each call site
     * against the decompiled source (e.g. moveBelowSeaLevel uses the
     * GENERATOR minY, JigsawPlacement padding and RuinedPortal findSuitableY
     * use the LEVEL).
     */
    virtual int32_t getLevelMinY() const { return getMinY(); }
    virtual int32_t getLevelHeight() const { return getGenDepth(); }

protected:
    /**
     * Get writable area bounding box for a chunk
     * Reference: ChunkGenerator.java lines 377-385
     */
    static void getWritableArea(
        const ::world::IChunk* chunk,
        int32_t& minX, int32_t& minY, int32_t& minZ,
        int32_t& maxX, int32_t& maxY, int32_t& maxZ
    );

    // Reference: WorldOptions.generateStructures() via StructureManager.
    bool m_generateStructures = false;
};

/**
 * NoiseBasedChunkGenerator - Noise-based terrain generation
 * Reference: NoiseBasedChunkGenerator.java (26.3)
 */
class NoiseBasedChunkGenerator : public ChunkGenerator {
public:
    explicit NoiseBasedChunkGenerator(std::shared_ptr<const NoiseGeneratorSettings> settings);
    ~NoiseBasedChunkGenerator() override;

    void buildTerrain(RandomState* randomState, const TerrainContext& context, ::world::IChunk* chunk) override;

    void setBiomeSource(world::biome::BiomeSource* biomeSource) { m_biomeSource = biomeSource; }

    int32_t getSeaLevel() const override;
    int32_t getMinY() const override;
    int32_t getGenDepth() const override;
    world::biome::BiomeSource* getBiomeSource() const { return m_biomeSource; }
    const NoiseGeneratorSettings* getSettings() const { return m_settings.get(); }

    // Single-biome (FixedBiomeSource) worlds build featuresPerStep from their
    // one biome, matching Java's per-generator possibleBiomes() build.
    const std::vector<StepFeatureData>* customFeaturesPerStep() override;

    /**
     * Reference: NoiseBasedChunkGenerator.getBaseHeight (iterateNoiseColumn).
     * Memoized: a pure function of (x, z, type) for a given generator, and
     * structure layout asks the same columns repeatedly. Bounded.
     */
    int32_t getBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                          RandomState* randomState) const override;

    /**
     * Reference: NoiseBasedChunkGenerator.getBaseColumn (iterateNoiseColumn).
     * outColumn[i] is the block at y = getMinY() + i.
     */
    void getBaseColumn(int32_t x, int32_t z, RandomState* randomState,
                       std::vector<BlockState*>& outColumn) const override;

    /**
     * Reference: ChunkGenerator.createBiomes / doCreateBiomes (26.3).
     */
    void createBiomes(RandomState* randomState, Blender* blender, ::world::IChunk* chunk) override;

    /**
     * Reference: NoiseBasedChunkGenerator.getOrigin (26.3): the spawn search
     * over the settings' spawn target (NoiseSpawnFinder); ChunkPos(0, 0) when
     * the settings have none.
     */
    ::world::ChunkPos getOrigin(RandomState* randomState) const;

    /**
     * Reference: NoiseBasedChunkGenerator.addDebugScreenInfo (26.3): the F3
     * "Density" line - each of the settings' debug_functions at the feet
     * block, "0.000" formatted.
     */
    void addDebugScreenInfo(std::vector<std::string>& result, RandomState* randomState, const core::BlockPos& feetPos,
                            density::SamplerContext& samplerContext) const;

    // The chunk's volume (NoiseBasedChunkGenerator.chunkVolume).
    std::unique_ptr<density::NoiseChunk> createNoiseChunk(::world::IChunk* chunk, RandomState& randomState,
                                                          std::shared_ptr<const density::DensitySampler> beardifier) const;

private:
    // NoiseBasedChunkGenerator.iterateNoiseColumn: fills `writeTo` (when not
    // null) and returns the top block matching `tester` + 1, or INT32_MIN.
    int32_t iterateNoiseColumn(int32_t blockX, int32_t blockZ, RandomState* randomState,
                               std::vector<BlockState*>* writeTo,
                               const std::function<bool(const BlockState*)>* tester) const;
    int32_t computeBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                              RandomState* randomState) const;

    void doFill(density::NoiseChunk& noiseChunk, ::world::IChunk* chunk) const;
    void buildSurface(RandomState& randomState, const TerrainContext& context, ::world::IChunk* chunk,
                      density::NoiseChunk& noiseChunk);
    void generateCarvers(RandomState& randomState, const TerrainContext& context, ::world::IChunk* chunk,
                         density::NoiseChunk& noiseChunk);
    void applyCarvingMask(::world::IChunk* chunk, const carver::CarvingMask& mask, RandomState& randomState,
                          carver::CarvingContext& context, density::NoiseChunk& noiseChunk,
                          const std::function<world::biome::BiomeHolder(const core::BlockPos&)>& biomeGetter);
    // The engine's post-surface passes for the Twilight Forest (the mod's
    // chunk blanket processors: dark-forest canopy, glacier).
    void twilightChunkBlanketing(const TerrainContext& context, ::world::IChunk* chunk);

    const material::MaterialRule* materialRule();

    std::shared_ptr<const NoiseGeneratorSettings> m_settings;
    world::biome::BiomeSource* m_biomeSource = nullptr;
    std::shared_ptr<const material::MaterialRule> m_materialRule;
    std::once_flag m_materialRuleOnce;

    mutable std::mutex m_baseHeightMutex;
    mutable std::unordered_map<uint64_t, int32_t> m_baseHeightCache;

    // Memoized featuresPerStep for single-biome (FixedBiomeSource) worlds -
    // Java builds featuresPerStep from possibleBiomes(), which is one biome
    // there, NOT the whole dimension. Empty until first use.
    std::vector<StepFeatureData> m_singleBiomeFeaturesPerStep;
    bool m_singleBiomeFeaturesBuilt = false;
};

} // namespace levelgen
} // namespace minecraft
