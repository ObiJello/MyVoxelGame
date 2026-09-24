#include "levelgen/carver/CarvingContext.h"

#include "levelgen/RandomState.h"
#include "levelgen/density/terrain/NoiseChunk.h"
#include "levelgen/material/MaterialSystem.h"
#include "world/biome/Biome.h"

// Reference: CarvingContext.topMaterial (26.3):
//   randomState.surfaceSystem().topMaterial(materialRule, randomState, this,
//       biomeGetter, chunk, noiseChunk.cachingSamplers(), pos, underFluid)

namespace minecraft {
namespace levelgen {
namespace carver {

BlockState* CarvingContext::topMaterial(const std::function<void*(const core::BlockPos&)>& biomeGetter,
                                        ::world::IChunk* chunk, const core::BlockPos& pos, bool underFluid) const {
    if (m_randomState == nullptr || m_noiseChunk == nullptr || m_materialRule == nullptr) return nullptr;
    material::MaterialSystem* surfaceSystem = m_randomState->surfaceSystem();
    if (surfaceSystem == nullptr) return nullptr;
    const material::BiomeGetter biomes = [&biomeGetter](const core::BlockPos& blockPos) {
        return static_cast<world::biome::BiomeHolder>(biomeGetter(blockPos));
    };
    return surfaceSystem->topMaterial(*m_materialRule, m_randomState->density(), *this, biomes, chunk,
                                      m_noiseChunk->cachingSamplers(), pos, underFluid);
}

} // namespace carver
} // namespace levelgen
} // namespace minecraft
