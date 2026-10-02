// File: src/common/world/block/ContactDamageBlocks.hpp
//
// The blocks that hurt what touches them (MC 26.3):
//
//   * CactusBlock.entityInside — `entity.hurt(cactus(), 1.0F)`, ANY entity
//     (a boat, a minecart and an End crystal break on one too). The cell
//     test is MC's default Shapes.block(), so the player held off by the
//     cactus's 1/16-inset collision box is still inside it. Dropped items
//     are not Game::Entities; ItemEntityManager runs their half
//     (ItemEntity.hurtServer: 5 health, gone in five ticks).
//   * SweetBerryBushBlock.entityInside — a LivingEntity other than a fox or
//     a bee, in a bush past AGE 0, that moved horizontally this tick
//     (>= 0.003 on x or z) takes sweetBerryBush() 1.0.
//   * CampfireBlock.entityInside — a lit campfire burns a LivingEntity for
//     its fireDamage (campfire 1, soul campfire 2) as campfire() damage.
//   * MagmaBlock.stepOn — a LivingEntity standing on it that is not
//     stepping carefully (sneaking) takes hotFloor() 1.0.
//   * PointedDripstoneBlock.fallOn — landing on a stalagmite tip; resolved
//     by the landing code itself (Entity::CheckFallDamage for mobs,
//     PlayerSession's move handling for players) through IsStalagmiteTip.
//
// All four are server-side (Entity.hurt is a no-op on a client level). Fire
// immunity, FIRE_RESISTANCE, Frost Walker (#burn_from_stepping) and the
// creative player's invulnerability are the hurt path's own business.
#pragma once

#include "BlockRegistry.hpp"

#include <array>

namespace Game {

    namespace PointedDripstone {

        // MC PointedDripstoneBlock.fallOn: landing on an UP-pointing TIP is
        // `causeFallDamage(fallDistance + 2.5, 2.0F, stalagmite())` IN PLACE
        // of Block.fallOn's plain fall damage (no fall-distance reduction).
        inline bool IsStalagmiteTip(BlockState state) {
            return state.Block() == BlockID::PointedDripstone &&
                   state.GetName(PropertyId::VERTICAL_DIRECTION) == "up" &&
                   state.GetName(PropertyId::THICKNESS) == "tip";
        }
        inline constexpr double kStalagmiteExtraFall        = 2.5;
        inline constexpr float  kStalagmiteDamageMultiplier = 2.0f;

    } // namespace PointedDripstone

    // Wires the hooks above. Called from BlockRegistry_RegisterBehaviors.
    void RegisterContactDamageBehaviors(std::array<Block, BlockRegistry::Size>& blocks);

} // namespace Game
