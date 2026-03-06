#pragma once

#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseRouter.h"
#include "world/level/block/state/BlockState.h"
#include <vector>

namespace minecraft {
namespace levelgen {

using BlockState = ::minecraft::world::level::block::state::BlockState;

// Forward declarations for dependencies we'll implement later
class SurfaceRuleSource; // For surface block generation
class ClimateParameterPoint; // For biome spawn points

// Random source algorithm enum
enum class RandomAlgorithm {
    LEGACY,     // Java's LCG (for Nether/End)
    XOROSHIRO   // Xoroshiro128++ (for Overworld)
};

class NoiseGeneratorSettings {
public:
    // Default constructor for overworld settings
    NoiseGeneratorSettings();

    // Constructor with all fields
    NoiseGeneratorSettings(
        const NoiseSettings& noiseSettings,
        BlockState* defaultBlock,
        BlockState* defaultFluid,
        const NoiseRouter& noiseRouter,
        SurfaceRuleSource* surfaceRule,
        const std::vector<ClimateParameterPoint*>& spawnTarget,
        int seaLevel,
        bool disableMobGeneration,
        bool aquifersEnabled,
        bool oreVeinsEnabled,
        bool useLegacyRandomSource
    );

    // Getters
    const NoiseSettings& noiseSettings() const { return m_noiseSettings; }
    BlockState* defaultBlock() const { return m_defaultBlock; }
    BlockState* defaultFluid() const { return m_defaultFluid; }
    NoiseRouter* noiseRouter() { return &m_noiseRouter; }
    const NoiseRouter* noiseRouter() const { return &m_noiseRouter; }
    SurfaceRuleSource* surfaceRule() const { return m_surfaceRule; }
    const std::vector<ClimateParameterPoint*>& spawnTarget() const { return m_spawnTarget; }
    int seaLevel() const { return m_seaLevel; }
    bool disableMobGeneration() const { return m_disableMobGeneration; }

    // Special getters with debug checks
    bool isAquifersEnabled() const;
    bool oreVeinsEnabled() const;
    RandomAlgorithm getRandomSource() const;

private:
    NoiseSettings m_noiseSettings;
    BlockState* m_defaultBlock;
    BlockState* m_defaultFluid;
    NoiseRouter m_noiseRouter;
    SurfaceRuleSource* m_surfaceRule;
    std::vector<ClimateParameterPoint*> m_spawnTarget;
    int m_seaLevel;
    bool m_disableMobGeneration;
    bool m_aquifersEnabled;
    bool m_oreVeinsEnabled;
    bool m_useLegacyRandomSource;
};

} // namespace levelgen
} // namespace minecraft
