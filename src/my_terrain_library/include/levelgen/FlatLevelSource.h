#pragma once

#include "levelgen/ChunkGenerator.h"
#include "levelgen/flat/FlatLevelGeneratorSettings.h"
#include "world/biome/FixedBiomeSource.h"

#include <memory>

// Reference: net/minecraft/world/level/levelgen/FlatLevelSource.java

namespace minecraft {
namespace levelgen {

/**
 * FlatLevelSource - the superflat chunk generator.
 * Reference: FlatLevelSource.java.
 *
 * Layers fill from the CHUNK's minY upward; surface/carvers/mob spawns are
 * no-ops; the biome source is a FixedBiomeSource of the settings' biome; the
 * feature lists are the settings' adjusted lists (see
 * FlatLevelGeneratorSettings); generator minY/genDepth are 0/384 (this feeds
 * WorldGenerationContext clamps for feature anchors, exactly like Java);
 * sea level is -63.
 */
class FlatLevelSource : public ChunkGenerator {
public:
    explicit FlatLevelSource(flat::FlatLevelGeneratorSettings settings);

    const flat::FlatLevelGeneratorSettings& settings() const { return m_settings; }
    world::biome::FixedBiomeSource* biomeSource() { return m_biomeSource.get(); }

    // Reference: FlatLevelSource.fillFromNoise().
    void fillFromNoise(RandomState* randomState, Blender* blender,
                       ::world::IChunk* chunk) override;

    // Reference: FlatLevelSource.applyCarvers() - no-op.
    void applyCarvers(int64_t /*seed*/, RandomState* /*randomState*/,
                      std::function<world::biome::BiomeHolder(const core::BlockPos&)> /*biomeGetter*/,
                      ::world::IChunk* /*chunk*/,
                      GenerationStep::Decoration /*step*/) override {}

    // Reference: FlatLevelSource.buildSurface() - no-op.
    void buildSurface(RandomState* /*randomState*/,
                      std::function<world::biome::BiomeHolder(const core::BlockPos&)> /*biomeGetter*/,
                      ::world::IChunk* /*chunk*/) override {}

    // Reference: ChunkGenerator.createBiomes() base implementation -
    // chunk.fillBiomesFromNoise(biomeSource, randomState.sampler()).
    void createBiomes(RandomState* randomState, Blender* blender,
                      ::world::IChunk* chunk) override;

    // Reference: FlatLevelSource.getSeaLevel/getMinY/getGenDepth.
    int32_t getSeaLevel() const override { return -63; }
    int32_t getMinY() const override { return 0; }
    int32_t getGenDepth() const override { return 384; }

    // Reference: FlatLevelSource.getBaseHeight().
    int32_t getBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                          RandomState* randomState) const override;

    // Reference: FlatLevelSource.getBaseColumn().
    void getBaseColumn(int32_t x, int32_t z, RandomState* randomState,
                       std::vector<BlockState*>& outColumn) const override;

    // Reference: Java flat getBaseColumn returns
    // NoiseColumn(heightAccessor.getMinY(), ...) - LEVEL-anchored, unlike the
    // noise generator (whose anchor equals its own minY).
    int32_t getBaseColumnMinY() const override { return m_levelMinY; }
    int32_t getLevelMinY() const override { return m_levelMinY; }
    int32_t getLevelHeight() const override { return m_levelHeight; }

    // The settings' adjusted feature lists stand in for Java's
    // generationSettingsGetter (only this generator's own biome is adjusted).
    const std::vector<std::vector<const placement::PlacedFeature*>>*
    featuresForBiomeOverride(const std::string& biomeKey) const override;

    bool hasFeatureInBiome(const std::string& biomeKey,
                           const placement::PlacedFeature* feature) const override;

    // featuresPerStep built from the adjusted lists of the single biome.
    const std::vector<StepFeatureData>* customFeaturesPerStep() override;

    // The level height range of the game/harness world hosting this
    // generator, needed by fillFromNoise (layers start at the CHUNK bottom)
    // and getBaseHeight/getBaseColumn (LevelHeightAccessor in Java). Set at
    // construction time by the embedder; defaults to the overworld range.
    void setLevelHeightRange(int32_t levelMinY, int32_t levelHeight) {
        m_levelMinY = levelMinY;
        m_levelHeight = levelHeight;
    }

private:
    flat::FlatLevelGeneratorSettings m_settings;
    std::unique_ptr<world::biome::FixedBiomeSource> m_biomeSource;
    std::vector<StepFeatureData> m_featuresPerStep;
    bool m_featuresPerStepBuilt = false;
    int32_t m_levelMinY = -64;
    int32_t m_levelHeight = 384;
};

} // namespace levelgen
} // namespace minecraft
