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
class NoiseChunk;
class SurfaceSystem;
class RuleSource;
class Beardifier;
class FluidPicker;
class NoiseGeneratorSettings;
class Aquifer;

namespace carver {
    class WorldCarverBase;
}

/**
 * ChunkGenerator - Abstract base for chunk generation
 * Reference: ChunkGenerator.java
 */
class ChunkGenerator {
public:
    virtual ~ChunkGenerator() = default;

    /**
     * Fill chunk with base terrain from noise
     * Reference: NoiseBasedChunkGenerator.java fillFromNoise() lines 233-337
     */
    virtual void fillFromNoise(
        RandomState* randomState,
        Blender* blender,
        ::world::IChunk* chunk
    ) = 0;

    /**
     * Apply carvers (caves, canyons) to a chunk
     * Reference: ChunkGenerator.java line 120
     */
    virtual void applyCarvers(
        int64_t seed,
        RandomState* randomState,
        std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
        ::world::IChunk* chunk,
        GenerationStep::Decoration step
    ) = 0;

    /**
     * Build surface blocks for a chunk
     * Reference: ChunkGenerator.java line 387
     * Reference: NoiseBasedChunkGenerator.java buildSurface() lines 192-196
     *
     * @param randomState - The random state for noise generation
     * @param biomeGetter - Function to get biome at a position (used for frozen ocean, eroded badlands)
     * @param chunk - The chunk to build surface on
     */
    virtual void buildSurface(
        RandomState* randomState,
        std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
        ::world::IChunk* chunk
    ) = 0;

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
     * keeps the historical per-dimension static builds in ChunkStatusTasks /
     * ChunkGenerationRunner (whose key ORDER is parity-proven). Generators
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
 * Reference: NoiseBasedChunkGenerator.java
 */
class NoiseBasedChunkGenerator : public ChunkGenerator {
private:
    int32_t m_seaLevel;
    int32_t m_minY;
    int32_t m_height;
    int32_t m_cellWidth;          // Horizontal cell size (default 4)
    int32_t m_cellHeight;         // Vertical cell size (default 8)
    SurfaceSystem* m_surfaceSystem;
    RuleSource* m_surfaceRules;
    BlockState* m_defaultBlock;  // Stone
    BlockState* m_airBlock;      // Air
    FluidPicker* m_fluidPicker;
    Beardifier* m_beardifier;
    NoiseGeneratorSettings* m_settings;   // Store settings for NoiseChunk creation
    world::biome::BiomeSource* m_biomeSource;  // Biome source for createBiomes

    // Memoized featuresPerStep for single-biome (FixedBiomeSource) worlds -
    // Java builds featuresPerStep from possibleBiomes(), which is one biome
    // there, NOT the whole dimension. Empty until first use.
    std::vector<StepFeatureData> m_singleBiomeFeaturesPerStep;
    bool m_singleBiomeFeaturesBuilt = false;

    /**
     * Internal terrain fill implementation
     * Reference: NoiseBasedChunkGenerator.java doFill() lines 263-337
     */
    void doFill(
        Blender* blender,
        RandomState* randomState,
        ::world::IChunk* chunk,
        int32_t cellMinY,
        int32_t cellCountY
    );

public:
    /**
     * Constructor with NoiseGeneratorSettings
     */
    NoiseBasedChunkGenerator(
        NoiseGeneratorSettings* settings,
        SurfaceSystem* surfaceSystem,
        RuleSource* surfaceRules,
        BlockState* defaultBlock,
        BlockState* airBlock,
        FluidPicker* fluidPicker,
        Beardifier* beardifier
    );

    /**
     * Legacy constructor (creates empty settings - not recommended)
     */
    NoiseBasedChunkGenerator(
        int32_t seaLevel,
        int32_t minY,
        int32_t height,
        int32_t cellWidth,
        int32_t cellHeight,
        SurfaceSystem* surfaceSystem,
        RuleSource* surfaceRules,
        BlockState* defaultBlock,
        BlockState* airBlock,
        FluidPicker* fluidPicker,
        Beardifier* beardifier
    );

    /**
     * Minimal constructor for compatibility
     */
    NoiseBasedChunkGenerator(
        int32_t seaLevel,
        int32_t minY,
        int32_t height,
        SurfaceSystem* surfaceSystem,
        RuleSource* surfaceRules
    );

    /**
     * Fill chunk with base terrain from noise
     * Reference: NoiseBasedChunkGenerator.java fillFromNoise() lines 233-261
     */
    void fillFromNoise(
        RandomState* randomState,
        Blender* blender,
        ::world::IChunk* chunk
    ) override;

    /**
     * Apply carvers
     */
    void applyCarvers(
        int64_t seed,
        RandomState* randomState,
        std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
        ::world::IChunk* chunk,
        GenerationStep::Decoration step
    ) override;

    /**
     * Build surface
     * Reference: NoiseBasedChunkGenerator.java buildSurface() lines 192-196
     */
    void buildSurface(
        RandomState* randomState,
        std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
        ::world::IChunk* chunk
    ) override;

    // Setters for late binding
    void setDefaultBlock(BlockState* block) { m_defaultBlock = block; }
    void setAirBlock(BlockState* block) { m_airBlock = block; }
    void setFluidPicker(FluidPicker* picker) { m_fluidPicker = picker; }
    void setBeardifier(Beardifier* beardifier) { m_beardifier = beardifier; }
    void setBiomeSource(world::biome::BiomeSource* biomeSource) { m_biomeSource = biomeSource; }

    // Accessors
    int32_t getSeaLevel() const override { return m_seaLevel; }
    int32_t getMinY() const override { return m_minY; }
    int32_t getGenDepth() const override { return m_height; }
    int32_t getCellWidth() const { return m_cellWidth; }
    int32_t getCellHeight() const { return m_cellHeight; }
    world::biome::BiomeSource* getBiomeSource() const { return m_biomeSource; }
    const NoiseGeneratorSettings* getSettings() const { return m_settings; }

    // Single-biome (FixedBiomeSource) worlds build featuresPerStep from their
    // one biome, matching Java's per-generator possibleBiomes() build.
    const std::vector<StepFeatureData>* customFeaturesPerStep() override;

    /**
     * Get base height at the given position
     * Reference: NoiseBasedChunkGenerator.java getBaseHeight()
     */
    int32_t getBaseHeight(
        int32_t x,
        int32_t z,
        Heightmap::Types heightmapType,
        RandomState* randomState
    ) const override;
    // Memo of getBaseHeight: a pure function of (x, z, type) for a given
    // generator, and structure layout asks the same columns repeatedly
    // (a village start = 273 calls x ~0.9 ms, 2026-08-30). Bounded.
    int32_t computeBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                              RandomState* randomState) const;
    mutable std::mutex m_baseHeightMutex;
    mutable std::unordered_map<uint64_t, int32_t> m_baseHeightCache;

    /**
     * Get a column of blocks at the given position
     * Reference: NoiseBasedChunkGenerator.java getBaseColumn()
     */
    void getBaseColumn(
        int32_t x,
        int32_t z,
        RandomState* randomState,
        std::vector<BlockState*>& outColumn
    ) const override;

    /**
     * Create biomes for a chunk
     * Reference: NoiseBasedChunkGenerator.java createBiomes()
     */
    void createBiomes(
        RandomState* randomState,
        Blender* blender,
        ::world::IChunk* chunk
    ) override;
};

} // namespace levelgen
} // namespace minecraft
