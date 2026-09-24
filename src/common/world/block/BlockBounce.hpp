// File: src/common/world/block/BlockBounce.hpp
//
// MC 26.3 BlockBehaviour.Properties.bounceRestitution /
// fallDistanceReduction, and the SUPPRESSES_BOUNCE block tag — the three
// per-block numbers Entity.restituteMovementAfterCollisions and
// Block.fallOn read when something lands. Blocks.java sets them on exactly
// three blocks:
//
//   bed (all sixteen dyes)  bounceRestitution 0.75  fallDistanceReduction 0.5
//   shelf_mushroom          bounceRestitution 0.75  fallDistanceReduction 0.5
//   slime_block             bounceRestitution 1.0   (fall damage is its own
//                           fallOn: cancelled outright unless sneaking)
//
// The straw bed has neither. honey_block is the whole SUPPRESSES_BOUNCE tag.
//
// Header-only: the local player's physics (Physics.cpp), mobs
// (LivingEntity.cpp) and the fall-damage path (Entity.cpp) all read these,
// and none of them wants a link dependency for three switches.
#pragma once

#include "Blocks.hpp"
#include "BedBlock.hpp"

namespace Game {

    // Block.getBounceRestitution — the fraction of the impact speed sent
    // back. 0 for everything that does not bounce.
    inline float BounceRestitution(BlockID id) {
        if (id == BlockID::SlimeBlock) return 1.0f;
        if (id == BlockID::ShelfMushroom) return 0.75f;
        if (IsBedBlock(id) && id != BlockID::StrawBed) return 0.75f;
        return 0.0f;
    }

    // Block.getFallDistanceReduction — Block.fallOn multiplies the fall
    // distance by (1 - this) before the damage is worked out.
    inline float FallDistanceReduction(BlockID id) {
        if (id == BlockID::ShelfMushroom) return 0.5f;
        // The Aether's AercloudBlock.fallOn is empty (no causeFallDamage at
        // all) for all three clouds — a full reduction is the same outcome
        // through this engine's one landing hook (Physics.cpp for the local
        // player, Entity::CheckFallDamage for everything else).
        if (id == BlockID::ColdAercloud || id == BlockID::BlueAercloud ||
            id == BlockID::GoldenAercloud) {
            return 1.0f;
        }
        if (IsBedBlock(id) && id != BlockID::StrawBed) return 0.5f;
        return 0.0f;
    }

    // BlockTags.SUPPRESSES_BOUNCE: landing on this never bounces, whatever
    // the entity's own bounciness says.
    inline bool SuppressesBounce(BlockID id) {
        return id == BlockID::HoneyBlock;
    }

} // namespace Game
