// File: src/common/world/block/BlockFriction.hpp
//
// MC Block.getFriction — BlockBehaviour.Properties.friction, 0.6 unless a
// block overrides it. Read by everything that slides on the ground: mobs
// (LivingEntity.travel), dropped items (ItemEntity.tick), experience orbs
// (ExperienceOrb.tick) and the local player (Physics.cpp HandleMovement,
// motion-aware so quicksoil's cap applies), always through
// Entity.getBlockPosBelowThatAffectsMyMovement, i.e. the block half a block
// (+ epsilon) below the entity's feet — see BlockPosBelowThatAffectsMovement.
#pragma once

#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>
#include <cmath>

namespace Game {

    inline float GetBlockFriction(BlockID id) {
        // Blocks.java: the only friction(...) overrides in the registry.
        switch (id) {
            case BlockID::Ice:
            case BlockID::PackedIce:
            case BlockID::FrostedIce:
                return 0.98f;
            case BlockID::BlueIce:
                return 0.989f;
            case BlockID::SlimeBlock:
                return 0.8f;
            // The Aether's QUICKSOIL: Properties.friction(1.1F) — above 1, so
            // a thing sliding on it speeds up. QuicksoilBlock is FrictionCapped;
            // callers that know the mover's motion use the overload below.
            case BlockID::Quicksoil:
            // QUICKSOIL_GLASS: the same friction(1.1F), also FrictionCapped
            // (QuicksoilGlassBlock implements FrictionCapped).
            case BlockID::QuicksoilGlass:
                return 1.1f;
            default:
                return 0.6f;
        }
    }

    // Aether FrictionCapped.getCappedFriction (QuicksoilBlock.getFriction):
    // once the mover is faster than 1 block a tick on either horizontal
    // axis, a friction-capped block answers 0.99 instead of its own value,
    // which is what stops quicksoil's 1.1 accelerating anything forever.
    // `motion` is blocks per tick. (The Boat half — clamping deltaRotation —
    // has no boat here to apply to.)
    inline float GetBlockFriction(BlockID id, const glm::dvec3& motion) {
        const float friction = GetBlockFriction(id);
        if ((id == BlockID::Quicksoil || id == BlockID::QuicksoilGlass) &&
            (std::abs(motion.x) > 1.0 || std::abs(motion.z) > 1.0)) {
            return 0.99f;
        }
        return friction;
    }

    // MC Entity.getBlockPosBelowThatAffectsMyMovement -> getOnPos(0.500001F):
    // half a block down plus an epsilon so an exact block boundary rounds
    // DOWN. That (and not the feet cell, nor getOnPosLegacy's 0.2) is which
    // block's slipperiness an entity resting on a slab above ice reads.
    inline glm::ivec3 BlockPosBelowThatAffectsMovement(const glm::dvec3& feet) {
        return glm::ivec3(static_cast<int>(std::floor(feet.x)),
                          static_cast<int>(std::floor(feet.y - 0.500001)),
                          static_cast<int>(std::floor(feet.z)));
    }

} // namespace Game
