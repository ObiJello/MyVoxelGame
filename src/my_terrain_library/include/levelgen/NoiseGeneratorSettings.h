#pragma once

#include "levelgen/density/terrain/TerrainSettings.h"
#include "world/level/block/state/BlockState.h"

#include <memory>
#include <string>
#include <utility>

// Reference: levelgen.NoiseGeneratorSettings (26.3). The record itself is
// density::TerrainSettings, decoded from worldgen/noise_settings (vanilla) or
// built in code (the engine's own dimensions, ModTerrainSettings); this class
// adds the engine's dimension tag and Java's derived accessors.

namespace minecraft {
namespace levelgen {

using BlockState = ::minecraft::world::level::block::state::BlockState;

// WorldgenRandom.Algorithm.
enum class RandomAlgorithm {
    LEGACY,     // Java's LCG (nether, end)
    XOROSHIRO   // Xoroshiro128++ (overworld)
};

class NoiseGeneratorSettings {
public:
    explicit NoiseGeneratorSettings(std::shared_ptr<const density::TerrainSettings> terrain,
                                    std::string dimensionTag = std::string())
        : m_terrain(std::move(terrain)), m_dimensionTag(std::move(dimensionTag)) {}

    // Registries.NOISE_SETTINGS entry ("minecraft:overworld", "minecraft:nether", ...).
    static std::shared_ptr<NoiseGeneratorSettings> load(const std::string& key);

    const density::TerrainSettings& terrain() const { return *m_terrain; }
    const std::shared_ptr<const density::TerrainSettings>& terrainPtr() const { return m_terrain; }

    const density::NoiseSettings& noiseSettings() const { return m_terrain->noiseSettings; }
    BlockState* defaultBlock() const { return m_terrain->defaultBlock; }
    BlockState* defaultFluid() const { return m_terrain->defaultFluid; }
    const density::NoiseRouter& noiseRouter() const { return m_terrain->noiseRouter; }
    const std::string& materialRule() const { return m_terrain->materialRule; }
    int seaLevel() const { return m_terrain->seaLevel; }
    bool disableMobGeneration() const { return m_terrain->disableMobGeneration; }
    bool isAquifersEnabled() const { return m_terrain->aquifers.has_value(); }
    bool useLegacyRandomSource() const { return m_terrain->useLegacyRandomSource; }
    RandomAlgorithm getRandomSource() const {
        return m_terrain->useLegacyRandomSource ? RandomAlgorithm::LEGACY : RandomAlgorithm::XOROSHIRO;
    }

    // Engine dimension tag (the DimensionGeneratorKey, e.g. "twilight_forest").
    // Empty for every dimension whose default block already identifies it
    // (Overworld stone, nether netherrack, end end_stone, Hush hushstone,
    // Aether holystone). The Twilight Forest's default block is vanilla stone
    // like the Overworld's, so the library's dimension sniff sites
    // (ChunkStatusTasks featuresPerStep, the
    // NoiseBasedChunkGenerator carvers and surface) key on this tag instead.
    // Not part of Java's NoiseGeneratorSettings: MC gets the same answer from
    // the generator's BiomeSource. Set once, before the generator is shared.
    const std::string& dimensionTag() const { return m_dimensionTag; }
    void setDimensionTag(std::string tag) { m_dimensionTag = std::move(tag); }
    bool isTwilightForest() const { return m_dimensionTag == "twilight_forest"; }

private:
    std::shared_ptr<const density::TerrainSettings> m_terrain;
    std::string m_dimensionTag;
};

} // namespace levelgen
} // namespace minecraft
