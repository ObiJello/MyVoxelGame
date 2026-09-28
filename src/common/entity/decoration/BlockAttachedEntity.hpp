// File: src/common/entity/decoration/BlockAttachedEntity.hpp
//
// MC net.minecraft.world.entity.decoration.BlockAttachedEntity — what every
// entity that lives on a block shares: the cell it is attached to
// ("block_pos"), the 100-tick "does my support still hold me" check, and
// breaking on any hit, push or move. HangingEntity (paintings, item frames)
// and Cushion derive from it, as in MC.
//
// Same architecture note as ArmorStand / EndCrystal: MC's block-attached
// entities are plain Entities; here they ride the Mob pipeline (Mob's
// NoAiTag) because the tracker, the wire, the NBT and the client store are
// Mob-shaped. Tick replaces Mob::Tick wholesale — nothing here ever moves.
//
// The places that must treat these as "not a LivingEntity" (a sweep attack,
// a name tag, a wither's target scan, the entity-interact fall-through) test
// for this class.
#pragma once

#include "common/entity/Mob.hpp"

#include <glm/glm.hpp>

namespace Game {

    class BlockAttachedEntity : public Mob {
    public:
        // MC BlockAttachedEntity.CHECK_INTERVAL.
        static constexpr int kCheckInterval = 100;

        // MC getPos — the attached cell.
        const glm::ivec3& GetBlockPos() const { return m_pos; }

        // MC survives(): may this entity stay where it is?
        virtual bool Survives() const = 0;

        // MC dropItem(level, causedBy): the break sound and the drops.
        virtual void DropItem(Entity* causedBy) = 0;

        // MC thunderHit: nothing on the base (lightning neither burns nor
        // breaks a painting or an item frame); the cushion overrides it.
        void ThunderHit(Entity* bolt) override { (void)bolt; }

        // ── Entity hooks ───────────────────────────────────────────────────
        // MC BlockAttachedEntity.tick — the server's alone: checkBelowWorld,
        // then every kCheckInterval ticks tickAtCheckInterval and survives().
        void Tick() override;
        // MC BlockAttachedEntity.hurtServer: past the guards, any hit takes
        // the entity down with its drops.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        // MC BlockAttachedEntity.skipAttackInteraction: a player's punch is
        // hurtOrSimulate(playerAttack, 0), and the attack goes no further.
        bool SkipAttackInteraction(Entity& source) override;
        // MC push / move: anything that would move it breaks it instead; an
        // explosion's push arrives after its damage already has.
        void Knockback(double, double, double) override {}
        bool IsPickable() const override { return true; }
        bool IsPushable() const override { return false; }
        bool IsAffectedByPotions() const override { return false; }
        bool IsAttackable() const override { return true; }
        // MC: blocksBuilding stays false for a block-attached entity.
        bool BlocksBuilding() const override { return false; }
        void CheckDespawn() override {}
        bool RemoveWhenFarAway(double) const override { return false; }

    protected:
        BlockAttachedEntity(EntityTypeId type, EntityLevel* level);

        // MC tickAtCheckInterval — before the survival test (the cushion's
        // fluid contact and fire check).
        virtual void TickAtCheckInterval() {}
        // The survival test the periodic check runs. MC's is survives(); a
        // painting answers its own relaxed rule (Painting::StillHangs).
        virtual bool SurvivesCheck() const { return Survives(); }

        // MC kill(level, attributedTo): removed as KILLED (onKilled first).
        void Kill(Entity* attributedTo = nullptr);
        virtual void OnKilled() {}

        glm::ivec3 m_pos{0};
        int        m_ticksSinceLastCheck = 0;
    };

} // namespace Game
