// File: src/common/entity/LivingEntity.hpp
//
// MC net.minecraft.world.entity.LivingEntity — movement, health and damage.
//
// The constants in the .cpp are the whole point of this file, so they are
// listed here too as a checklist for anyone tempted to "tune" one:
//
//   gravity                 0.08   per tick   (Attributes.GRAVITY)
//   vertical drag           0.98              (travelInAir)
//   horizontal friction     blockFriction * 0.91
//   block friction          0.6 default, 0.98 ice, 0.989 blue ice, 0.8 slime
//   ground accel scale      speed * 0.21600002 / friction^3
//   air accel               0.02              (getFlyingSpeed)
//   jump velocity           0.42              (Attributes.JUMP_STRENGTH)
//   sprint-jump bonus       0.2 along facing
//   motion deadzone         0.003 per axis
//   input decay             xxa/zza *= 0.98 each tick
//   invulnerable window     20 ticks, hurt flash 10
//   knockback               0.4
//
// The 0.21600002 is not a typo and must not be rounded: it is MC's literal,
// and the cube of the friction in the denominator makes small differences
// visible as a walk speed that drifts away from vanilla over distance.
#pragma once

#include "common/entity/Entity.hpp"
#include "common/entity/Attributes.hpp"

#include <memory>

namespace Game {

    // MC WalkAnimationState — drives the limb swing. Lives on the entity
    // because the renderer reads it with a partial tick, not just per tick.
    struct WalkAnimationState {
        float speedOld = 0.0f;
        float speed    = 0.0f;
        float position = 0.0f;
        float positionScale = 1.0f;

        void Update(float targetSpeed, float factor, float scale) {
            speedOld = speed;
            speed += (targetSpeed - speed) * factor;
            position += speed;
            positionScale = scale;
        }
        void Stop() { speedOld = 0.0f; speed = 0.0f; }

        // MC WalkAnimationState.isMoving. The frog's idle-in-water animation
        // plays only while this is false, so the threshold is load-bearing:
        // `speed != 0` would never be satisfied once the smoothing has left a
        // trailing fraction behind.
        bool IsMoving() const { return speed > 1.0e-5f; }

        float SpeedAt(float partialTick) const {
            const float s = speedOld + partialTick * (speed - speedOld);
            return s < 1.0f ? s : 1.0f;
        }
        float PositionAt(float partialTick) const {
            return (position - speed * (1.0f - partialTick)) * positionScale;
        }
    };

    // Why something took damage. Only the sources this engine can actually
    // produce; MC's full DamageType registry is not modelled.
    enum class MobDamageSource : uint8_t {
        Generic = 0,
        MobAttack,
        PlayerAttack,
        Projectile,
        Fall,
        Fire,
        Drown,
        Explosion,
        Void,
    };

    class Brain;

    class LivingEntity : public Entity {
    public:
        LivingEntity(EntityTypeId type, EntityLevel* level);
        // Out of line so that unique_ptr<Brain> only needs Brain to be complete
        // in LivingEntity.cpp — every translation unit that merely holds a
        // LivingEntity would otherwise have to include the whole brain.
        ~LivingEntity() override;

        // ── Brain (MC LivingEntity.brain) ──────────────────────────────────
        //
        // Null for every mob MC drives with goals, which is most of them. MC
        // gives all of them a brain object and leaves it empty; a null pointer
        // says the same thing without paying for 116 memory slots on every
        // zombie in the world.
        Brain*       GetBrain()       { return m_brain.get(); }
        const Brain* GetBrain() const { return m_brain.get(); }

        // ── Attributes ─────────────────────────────────────────────────────
        AttributeMap&       Attributes()       { return m_attributes; }
        const AttributeMap& Attributes() const { return m_attributes; }
        double GetAttributeValue(Attribute a) const { return m_attributes.GetValue(a); }

        // ── Health ─────────────────────────────────────────────────────────
        float GetHealth() const { return m_health; }
        void  SetHealth(float h);
        float GetMaxHealth() const { return static_cast<float>(GetAttributeValue(Attribute::MaxHealth)); }
        bool  IsDeadOrDying() const { return m_health <= 0.0f; }
        bool  IsAlive() const override { return !IsRemoved() && m_health > 0.0f; }

        // MC LivingEntity.hurtServer. Returns true when damage actually landed
        // — callers use that to decide whether to apply knockback and effects.
        //
        // Runs the full MC sequence including the two-stage invulnerability
        // rule: inside the window a NEW hit only lands if it exceeds the one
        // that opened the window, and then only for the difference.
        virtual bool Hurt(MobDamageSource source, float amount, Entity* attacker);

        // MC LivingEntity.knockback. `dx`/`dz` point FROM the attacker TOWARD
        // this entity's push direction (MC passes attackerX - myX, which sends
        // the victim away from the attacker).
        virtual void Knockback(double power, double dx, double dz);

        virtual void Die(MobDamageSource source, Entity* attacker);
        virtual void TickDeath();

        // ── Damage bookkeeping (read by HurtByTargetGoal) ──────────────────
        Entity* GetLastHurtByMob() const { return m_lastHurtByMob; }
        void    SetLastHurtByMob(Entity* e);
        int64_t GetLastHurtByMobTimestamp() const { return m_lastHurtByMobTimestamp; }
        MobDamageSource GetLastDamageSource() const { return m_lastDamageSource; }
        bool    HasLastDamageSource() const { return m_hasLastDamageSource; }

        Entity* GetLastHurtMob() const { return m_lastHurtMob; }
        void    SetLastHurtMob(Entity* e) { m_lastHurtMob = e; }

        // Drop cached pointers to an entity that is about to be destroyed.
        // See Goal::ClearReferenceTo for why this is needed at all; these two
        // fields are the LivingEntity-level equivalent, and HurtByTargetGoal
        // reads m_lastHurtByMob directly.
        virtual void ClearReferenceTo(const Entity* entity);

        // ── Movement inputs (set by AI or by a player controller) ──────────
        float xxa = 0.0f;   // strafe, + is right
        float yya = 0.0f;   // up, used only in fluids/flight
        float zza = 0.0f;   // forward
        bool  jumping = false;

        float GetSpeed() const { return m_speed; }
        virtual void SetSpeed(float s) { m_speed = s; }

        // MC LivingEntity.discardFriction. While set, travel applies NEITHER
        // horizontal friction NOR the 0.98 vertical drag — which is the whole
        // reason a frog's long jump carries it four blocks instead of being
        // eaten by air resistance halfway. Server-side only, exactly as in MC:
        // the client's copy of the mob leaves it false, and the periodic
        // position packets correct the difference.
        bool ShouldDiscardFriction() const { return m_discardFriction; }
        void SetDiscardFriction(bool v) { m_discardFriction = v; }

        // ── Rotations beyond the base's yRot/xRot ──────────────────────────
        float yHeadRot  = 0.0f;
        float yHeadRotO = 0.0f;
        float yBodyRot  = 0.0f;
        float yBodyRotO = 0.0f;

        // ── Hurt / attack animation state (read by the renderer) ──────────
        int   hurtTime     = 0;   // counts down from 10 — drives the red flash
        int   hurtDuration = 0;
        int   deathTime    = 0;   // counts up to 20 — drives the death fall
        float attackAnim   = 0.0f;
        float oAttackAnim  = 0.0f;
        bool  swinging     = false;
        int   swingTime    = 0;

        WalkAnimationState walkAnimation;

        void Swing();

        // ── Movement ───────────────────────────────────────────────────────
        double GetGravity() const override { return GetAttributeValue(Attribute::Gravity); }
        float  MaxUpStep()  const override { return static_cast<float>(GetAttributeValue(Attribute::StepHeight)); }

        // MC LivingEntity.travel — the whole locomotion step.
        virtual void Travel(const glm::dvec3& input);

        virtual void JumpFromGround();
        virtual float GetJumpPower() const;

        // MC LivingEntity.isImmobile — a dead or sleeping entity keeps its
        // physics but stops steering.
        virtual bool IsImmobile() const { return IsDeadOrDying(); }

        // MC LivingEntity.isEffectiveAi — false on the client, which is what
        // stops a client-side mob from running goals while still letting it
        // fall, animate and play its death sequence.
        virtual bool IsEffectiveAi() const;

        virtual void AiStep();
        void Tick() override;
        void BaseTick() override;

        // The timer half of MC LivingEntity.baseTick: the hurt flash, the
        // invulnerability window and the 100-tick memory of who last hurt us.
        // Split out because the server's player view is NOT ticked as an entity
        // (the client owns player movement) but still has to count these down —
        // see PlayerEntityView::TickCombatState.
        void TickCombatTimers();

        // MC Mob.getMaxHeadXRot / getMaxHeadYRot / getHeadRotSpeed. On
        // LivingEntity so LookControl can be written against this type.
        virtual int GetMaxHeadXRot() const { return 40; }
        virtual int GetMaxHeadYRot() const { return 75; }
        virtual int GetHeadRotSpeed() const { return 10; }

        // MC LivingEntity.getYHeadRot / setYHeadRot.
        float GetYHeadRot() const { return yHeadRot; }
        void  SetYHeadRot(float v) { yHeadRot = v; }

        // Can this entity be targeted/attacked by `attacker`? Overridden by
        // Animal (never targets), and by the player adapter for creative and
        // spectator mode.
        virtual bool IsAttackable() const { return true; }

        // MC LivingEntity.calculateEntityAnimation — advances the walk
        // animation from the distance travelled this tick. Client-side only in
        // MC, and the same here: the server has no renderer to feed.
        void CalculateEntityAnimation(bool useY);

    protected:
        bool m_discardFriction = false;

        // Owned here because MC owns it on LivingEntity, and because a mob's
        // behaviours hold raw pointers back to it.
        std::unique_ptr<Brain> m_brain;

        // MC LivingEntity.actuallyHurt — armor, absorption and the health
        // subtraction, after Hurt has decided the damage lands.
        virtual void ActuallyHurt(MobDamageSource source, float amount, Entity* attacker);

        virtual float GetDamageAfterArmorAbsorb(MobDamageSource source, float amount) const;

        // MC LivingEntity.tickHeadTurn — Mob overrides this to run the body
        // rotation control instead.
        virtual void TickHeadTurn(float yBodyRotTarget);

        // MC LivingEntity.serverAiStep — empty here, final on Mob.
        virtual void ServerAiStep() {}

        // MC LivingEntity.updateWalkAnimation. Overridden by Chicken? No — by
        // nothing among our eight, but kept virtual to match MC.
        virtual void UpdateWalkAnimation(float distance);

        void UpdateSwingTime();

        AttributeMap m_attributes;

        float m_health = 20.0f;
        float m_speed  = 0.0f;

        int   m_invulnerableTime = 0;
        float m_lastHurt = 0.0f;
        int   m_noJumpDelay = 0;
        bool  m_dead = false;

        Entity*         m_lastHurtByMob = nullptr;
        int64_t         m_lastHurtByMobTimestamp = 0;
        Entity*         m_lastHurtMob = nullptr;
        MobDamageSource m_lastDamageSource = MobDamageSource::Generic;
        bool            m_hasLastDamageSource = false;
    };

    // MC Block.getFriction. Exposed because the pathfinder and the mob mover
    // both need it and neither owns the table.
    float GetBlockFriction(BlockID id);

} // namespace Game
