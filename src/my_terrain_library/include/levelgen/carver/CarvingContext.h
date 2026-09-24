#pragma once

#include "levelgen/WorldGenerationContext.h"
#include "core/BlockPos.h"
#include "world/IChunk.h"
#include "world/level/block/state/BlockState.h"

#include <functional>

// Reference: net/minecraft/world/level/levelgen/carver/CarvingContext.java (26.3)

namespace minecraft {
namespace levelgen {

class RandomState;
namespace density {
class NoiseChunk;
}
namespace material {
class MaterialRule;
}

namespace carver {

using BlockState = ::minecraft::world::level::block::state::BlockState;

/**
 * CarvingContext - the WorldGenerationContext carvers resolve their anchors
 * in, plus what the top-material pass needs (the chunk's NoiseChunk, the
 * RandomState and the dimension's material rule).
 */
class CarvingContext : public WorldGenerationContext {
public:
    // The vanilla 26.3 carvers only mark the mask (applyCarvingMask writes
    // it); the mod carvers still write their cells directly.
    bool maskOnly = false;

    CarvingContext(int32_t minY, int32_t height, density::NoiseChunk* noiseChunk, RandomState* randomState,
                   const material::MaterialRule* materialRule)
        : WorldGenerationContext(minY, height),
          m_noiseChunk(noiseChunk),
          m_randomState(randomState),
          m_materialRule(materialRule) {}

    /**
     * Reference: CarvingContext.topMaterial -> MaterialSystem.topMaterial:
     * the material rules' answer for a dirt block a carver exposed under
     * grass. nullptr = Optional.empty().
     */
    BlockState* topMaterial(const std::function<void*(const core::BlockPos&)>& biomeGetter, ::world::IChunk* chunk,
                            const core::BlockPos& pos, bool underFluid) const;

    RandomState* randomState() const { return m_randomState; }
    density::NoiseChunk* noiseChunk() const { return m_noiseChunk; }

private:
    density::NoiseChunk* m_noiseChunk;
    RandomState* m_randomState;
    const material::MaterialRule* m_materialRule;
};

} // namespace carver
} // namespace levelgen
} // namespace minecraft
