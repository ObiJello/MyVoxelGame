// File: src/common/entity/FallingBlockLanding.hpp
//
// The landing half of MC FallingBlockEntity.tick(), as a free function over
// plain values — no entity required.
//
// Two callers share it and MUST keep sharing it: FallingBlockEntity::TryLand
// (the Mob-shaped falling block: anvils, concrete powder, dripstone, anything
// summoned or loaded from NBT) and Server::FallingBlockStore (the compact
// struct-of-arrays representation mass-falling sand/gravel takes). The whole
// point of putting the logic here is that the two representations cannot
// drift apart in what "landing" means.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>

namespace Game {

    class ILevelWrite;

    enum class FallingBlockLandOutcome : uint8_t {
        Placed,     // became a block; the entity is done
        Dropped,    // could not place, popped as an item; the entity is done
        Broken,     // could not place, no item (cancelDrop / drops off); done
        Retry       // the world refused the write and nothing drops — MC
                    // simply tries again next tick; the entity lives on
    };

    // MC FallingBlockEntity.tick's `else` branch after the landing damp: the
    // three questions (may I replace what is here, would I keep falling,
    // can this block survive here), the placement with water re-logging,
    // and the drop-or-break fallbacks. `position` is the entity's exact
    // position, which is where MC's spawnAtLocation pops the item.
    FallingBlockLandOutcome FallingBlockTryLand(ILevelWrite& level, const glm::ivec3& pos,
                                                BlockState state, const glm::dvec3& position,
                                                bool dropItem, bool cancelDrop,
                                                bool doEntityDrops);

} // namespace Game
