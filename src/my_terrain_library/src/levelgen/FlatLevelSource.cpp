#include "levelgen/FlatLevelSource.h"

#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/RandomState.h"
#include "world/ProtoChunk.h"

#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/FlatLevelSource.java

namespace minecraft {
namespace levelgen {

FlatLevelSource::FlatLevelSource(flat::FlatLevelGeneratorSettings settings)
    : m_settings(std::move(settings)),
      m_biomeSource(std::make_unique<world::biome::FixedBiomeSource>(m_settings.biome())) {}

void FlatLevelSource::fillFromNoise(RandomState* /*randomState*/, Blender* /*blender*/,
                                    ::world::IChunk* chunk) {
    // Reference: FlatLevelSource.fillFromNoise() - one layer per y from the
    // CHUNK's minY, updating the two worldgen heightmaps as it goes. Null
    // layers (non-opaque, moved to FILL_LAYER features) are skipped.
    auto* protoChunk = dynamic_cast<::world::ProtoChunk*>(chunk);
    if (!protoChunk) return;

    const auto& layers = m_settings.layers();
    Heightmap& oceanFloor =
        protoChunk->getOrCreateHeightmap(Heightmap::Types::OCEAN_FLOOR_WG);
    Heightmap& worldSurface =
        protoChunk->getOrCreateHeightmap(Heightmap::Types::WORLD_SURFACE_WG);

    int32_t chunkMinY = chunk->getMinBuildHeight();
    int32_t chunkHeight = chunk->getMaxBuildHeight() - chunkMinY;
    int32_t count = std::min<int32_t>(chunkHeight, static_cast<int32_t>(layers.size()));

    for (int32_t layerIndex = 0; layerIndex < count; ++layerIndex) {
        BlockState* state = layers[static_cast<size_t>(layerIndex)];
        if (state == nullptr) continue;
        int32_t y = chunkMinY + layerIndex;
        for (int32_t x = 0; x < 16; ++x) {
            for (int32_t z = 0; z < 16; ++z) {
                protoChunk->setBlockState(x, y, z, state, false);
                oceanFloor.update(x, y, z, state);
                worldSurface.update(x, y, z, state);
            }
        }
    }
}

void FlatLevelSource::createBiomes(RandomState* randomState, Blender* /*blender*/,
                                   ::world::IChunk* chunk) {
    // Reference: ChunkGenerator.createBiomes() base implementation.
    auto* protoChunk = dynamic_cast<::world::ProtoChunk*>(chunk);
    if (!protoChunk || !randomState || !randomState->sampler()) return;
    protoChunk->fillBiomesFromNoise(m_biomeSource.get(), *randomState->sampler());
}

int32_t FlatLevelSource::getBaseHeight(int32_t /*x*/, int32_t /*z*/,
                                       Heightmap::Types heightmapType,
                                       RandomState* /*randomState*/) const {
    // Reference: FlatLevelSource.getBaseHeight() - highest opaque layer + 1
    // for the heightmap's predicate; heightAccessor is the LEVEL.
    const auto& layers = m_settings.layers();
    auto isOpaque = Heightmap::getOpaquePredicate(heightmapType);
    int32_t levelMaxY = m_levelMinY + m_levelHeight - 1;  // inclusive
    int32_t start = std::min<int32_t>(static_cast<int32_t>(layers.size()) - 1, levelMaxY);
    for (int32_t layerIndex = start; layerIndex >= 0; --layerIndex) {
        BlockState* state = layers[static_cast<size_t>(layerIndex)];
        if (state != nullptr && isOpaque(state)) {
            return m_levelMinY + layerIndex + 1;
        }
    }
    return m_levelMinY;
}

void FlatLevelSource::getBaseColumn(int32_t /*x*/, int32_t /*z*/,
                                    RandomState* /*randomState*/,
                                    std::vector<BlockState*>& outColumn) const {
    // Reference: FlatLevelSource.getBaseColumn() - the layer stack from the
    // level's minY, null layers as air. Column index = y - level minY (the
    // convention the structure code uses).
    const auto& layers = m_settings.layers();
    BlockState* air = world::level::block::Blocks::AIR->defaultBlockState();
    outColumn.assign(static_cast<size_t>(m_levelHeight), air);
    size_t count = std::min<size_t>(outColumn.size(), layers.size());
    for (size_t i = 0; i < count; ++i) {
        outColumn[i] = layers[i] != nullptr ? layers[i] : air;
    }
}

const std::vector<std::vector<const placement::PlacedFeature*>>*
FlatLevelSource::featuresForBiomeOverride(const std::string& biomeKey) const {
    // Reference: adjustGenerationSettings(sourceBiome) - only this settings'
    // own biome is adjusted; any other biome falls back to its vanilla lists
    // (cannot occur with a FixedBiomeSource, but Java handles it the same way).
    if (biomeKey == m_settings.biome()) {
        return &m_settings.adjustedFeatures();
    }
    return nullptr;
}

bool FlatLevelSource::hasFeatureInBiome(const std::string& biomeKey,
                                        const placement::PlacedFeature* feature) const {
    if (biomeKey == m_settings.biome()) {
        for (const auto& step : m_settings.adjustedFeatures()) {
            for (const placement::PlacedFeature* f : step) {
                if (f == feature) return true;
            }
        }
        return false;
    }
    return ChunkGenerator::hasFeatureInBiome(biomeKey, feature);
}

const std::vector<StepFeatureData>* FlatLevelSource::customFeaturesPerStep() {
    // Reference: ChunkGenerator.featuresPerStep from possibleBiomes() = the
    // one fixed biome, with this generator's adjusted feature lists.
    if (!m_featuresPerStepBuilt) {
        std::vector<std::string> keys{m_settings.biome()};
        m_featuresPerStep = FeatureSorter::buildFeaturesPerStep<std::string>(
            keys,
            [this](const std::string& biomeKey)
                -> std::vector<std::vector<placement::PlacedFeature*>> {
                const auto* adjusted = featuresForBiomeOverride(biomeKey);
                const auto& features = adjusted
                    ? *adjusted
                    : data::worldgen::BiomeFeatureRegistry::getFeaturesForBiome(biomeKey);
                std::vector<std::vector<placement::PlacedFeature*>> result;
                result.reserve(features.size());
                for (const auto& stepFeatures : features) {
                    std::vector<placement::PlacedFeature*> step;
                    step.reserve(stepFeatures.size());
                    for (const auto* f : stepFeatures) {
                        step.push_back(const_cast<placement::PlacedFeature*>(f));
                    }
                    result.push_back(std::move(step));
                }
                return result;
            },
            true);
        m_featuresPerStepBuilt = true;
    }
    return &m_featuresPerStep;
}

} // namespace levelgen
} // namespace minecraft
