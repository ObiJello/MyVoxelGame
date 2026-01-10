#include "levelgen/NoiseGeneratorSettings.h"
#include "world/MinecraftBlockType.h"

namespace minecraft {
namespace levelgen {

// Debug flags (matching SharedConstants in Java)
static const bool DEBUG_DISABLE_AQUIFERS = false;
static const bool DEBUG_DISABLE_ORE_VEINS = false;

NoiseGeneratorSettings::NoiseGeneratorSettings()
    : m_noiseSettings(NoiseSettings::OVERWORLD_NOISE_SETTINGS)
    , m_defaultBlock(::world::MinecraftBlocks::STONE())
    , m_defaultFluid(::world::MinecraftBlocks::WATER())
    , m_noiseRouter()
    , m_surfaceRule(nullptr)
    , m_spawnTarget()
    , m_seaLevel(63)
    , m_disableMobGeneration(false)
    , m_aquifersEnabled(true)
    , m_oreVeinsEnabled(true)
    , m_useLegacyRandomSource(false)
{
}

NoiseGeneratorSettings::NoiseGeneratorSettings(
    const NoiseSettings& noiseSettings,
    ::world::IBlockType* defaultBlock,
    ::world::IBlockType* defaultFluid,
    const NoiseRouter& noiseRouter,
    SurfaceRuleSource* surfaceRule,
    const std::vector<ClimateParameterPoint*>& spawnTarget,
    int seaLevel,
    bool disableMobGeneration,
    bool aquifersEnabled,
    bool oreVeinsEnabled,
    bool useLegacyRandomSource
)
    : m_noiseSettings(noiseSettings)
    , m_defaultBlock(defaultBlock)
    , m_defaultFluid(defaultFluid)
    , m_noiseRouter(noiseRouter)
    , m_surfaceRule(surfaceRule)
    , m_spawnTarget(spawnTarget)
    , m_seaLevel(seaLevel)
    , m_disableMobGeneration(disableMobGeneration)
    , m_aquifersEnabled(aquifersEnabled)
    , m_oreVeinsEnabled(oreVeinsEnabled)
    , m_useLegacyRandomSource(useLegacyRandomSource)
{
}

bool NoiseGeneratorSettings::isAquifersEnabled() const {
    // Java: return this.aquifersEnabled && !SharedConstants.DEBUG_DISABLE_AQUIFERS;
    return m_aquifersEnabled && !DEBUG_DISABLE_AQUIFERS;
}

bool NoiseGeneratorSettings::oreVeinsEnabled() const {
    // Java: return this.oreVeinsEnabled && !SharedConstants.DEBUG_DISABLE_ORE_VEINS;
    return m_oreVeinsEnabled && !DEBUG_DISABLE_ORE_VEINS;
}

RandomAlgorithm NoiseGeneratorSettings::getRandomSource() const {
    // Java: return this.useLegacyRandomSource ? WorldgenRandom.Algorithm.LEGACY : WorldgenRandom.Algorithm.XOROSHIRO;
    return m_useLegacyRandomSource ? RandomAlgorithm::LEGACY : RandomAlgorithm::XOROSHIRO;
}

} // namespace levelgen
} // namespace minecraft
