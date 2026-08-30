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

#include "common/core/EntityRef.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/effect/MobEffects.hpp"

#include <cmath>
#include <memory>
#include <vector>

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
        // MC WalkAnimationState.stop zeroes the POSITION too — a dying mob
        // relaxes to the rest pose for the topple instead of freezing
        // mid-stride.
        void Stop() { speedOld = 0.0f; speed = 0.0f; position = 0.0f; }
        // MC setSpeed — the client-side hurt flinch writes 1.5 directly.
        void SetSpeed(float s) { speed = s; }

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
        Magic,    // MC "magic"/"indirect_magic" — poison ticks, harming potions
        Wither,   // MC "wither" — the wither effect's tick
        Cramming, // MC "cramming" — the maxEntityCramming overflow damage
        // MC DamageTypes.FALLING_BLOCK / FALLING_ANVIL / FALLING_STALACTITE /
        // STALAGMITE. Kept as four sources rather than one because vanilla's
        // death messages distinguish them ("was squashed by a falling anvil"
        // reads very differently from "was squashed by a falling block"), and
        // because the anvil source is the one armour is supposed to blunt.
        FallingBlock,
        FallingAnvil,
        FallingStalactite,
        Stalagmite,
    };

    // MC's `#minecraft:no_knockback` damage-type tag
    // (data/minecraft/tags/damage_type/no_knockback.json).
    //
    // A hit from one of these does NOT shove the victim away from its
    // attacker. That matters most for EXPLOSIONS: a blast's only push is the
    // exposure-scaled one the explosion itself applies, so standing behind an
    // obsidian wall means you are not moved at all. Without this the generic
    // "knock away from whoever hurt you" in LivingEntity::Hurt fires as well,
    // and it has no line-of-sight test — which is exactly how you get launched
    // through a wall the blast could not see you past.
    //
    // The other members are the ambient/environmental sources, where being
    // shoved makes no sense: fire, lava, drowning, starving, cramming, fall
    // damage, the void, poison/wither ticks and landing on a stalagmite.
    //
    // NOT in the tag, and therefore still knocking back: mob and player melee,
    // projectiles, and the three falling-block sources — an anvil landing on
    // you does push you.
    // MC DamageType's `scaling` field, from data/minecraft/damage_type/*.json.
    //
    // A source that scales runs through Player.hurtServer's difficulty pass:
    //     PEACEFUL -> damage = 0        (and the hit is then DROPPED entirely)
    //     EASY     -> min(dmg/2 + 1, dmg)
    //     NORMAL   -> unchanged
    //     HARD     -> dmg * 3/2
    //
    // Only four vanilla types are `"scaling": "always"`, and EXPLOSION is one
    // of them (with player_explosion, bad_respawn_point and sonic_boom). Every
    // other type is "when_caused_by_living_non_player", which needs a living
    // non-player attacker — a condition nothing in this engine's damage paths
    // currently satisfies, since mob melee comes through as MobAttack from a
    // Mob but the distinction is not modelled here.
    //
    // This matters for the explosion's `+1` damage floor: MC gives every entity
    // in range at least 1 damage no matter how well covered, and on PEACEFUL
    // that 1 is scaled to 0 and the hit never lands. Without this, hiding
    // behind obsidian on Peaceful still costs half a heart.
    inline bool DamageSourceScalesWithDifficulty(MobDamageSource source) {
        return source == MobDamageSource::Explosion;
    }

    inline bool DamageSourceHasNoKnockback(MobDamageSource source) {
        switch (source) {
            case MobDamageSource::Explosion:
            case MobDamageSource::Fire:
            case MobDamageSource::Drown:
            case MobDamageSource::Cramming:
            case MobDamageSource::Fall:
            case MobDamageSource::Void:
            case MobDamageSource::Generic:
            case MobDamageSource::Magic:
            case MobDamageSource::Wither:
            case MobDamageSource::Stalagmite:
                return true;
            default:
                return false;
        }
    }

    class Brain;

    class LivingEntity : public Entity {
    public:
        LivingEntity(EntityTypeId type, EntityLevel* level);

    protected:
        // ── The no-attributes constructor ───────────────────────────────────
        //
        // For the Mob-shaped entities that are not mobs — primed TNT and
        // falling blocks (see Mob::NoAiTag). CreateLivingAttributes registers
        // thirteen AttributeInstances, and EVERY base value it registers is
        // identical to that attribute's kAttributeTable default. AttributeMap's
        // readers fall back to that default for an unregistered attribute
        // (GetValue / GetBaseValue) and materialise one on demand for a writer
        // (SetBaseValue / AddModifier), so an entity with an empty map answers
        // every query exactly as a fully-registered one would. The registration
        // is pure cost, and at a hundred thousand primed TNT it is the vector
        // growth plus ~91 linear Find comparisons per entity.
        //
        // It is also what wrote max_health / movement_speed / armor into every
        // entity in entities/*.mca — thirteen NBT compounds per TNT, none of
        // which carried information (WriteAttributes serialises Attributes()
        // .All()). An empty map writes nothing, and ReadAttributes already
        // skips any attribute the entity does not Has(), so old saves still
        // load.
        struct NoAttributesTag {};
        LivingEntity(EntityTypeId type, EntityLevel* level, NoAttributesTag);

    public:

        LivingEntity* AsLiving() override { return this; }
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

        // ── Status effects (MC LivingEntity.activeEffects) ─────────────────
        //
        // SERVER-SIDE this wave: nothing syncs an effect to the client (there
        // is no particle system to render the swirls anyway); the documented
        // follow-up is a per-entity effect byte alongside the anim byte. All
        // gameplay consequences — damage ticks, attribute modifiers, the
        // travel/jump/fall hooks — run on the server's copy, which is the
        // authoritative one for every one of them.

        // MC LivingEntity.addEffect(effect, source). Returns whether the
        // effect landed or upgraded an existing instance (MobEffectInstance
        // .update rules). `source` is MC's attribution entity; nothing here
        // consumes it yet, but every call site passes what MC passes so the
        // wiring is already right when something does.
        bool AddEffect(MobEffectInstance effect, Entity* source = nullptr);
        bool RemoveEffect(MobEffectId effect);
        bool RemoveAllEffects();
        bool HasEffect(MobEffectId effect) const { return GetEffect(effect) != nullptr; }
        const MobEffectInstance* GetEffect(MobEffectId effect) const;
        // Loading installs saved instances DIRECTLY. AddEffect applies MC's
        // update/upgrade rules (a stronger effect replaces a weaker one, an
        // equal one extends it), which is right for gameplay and wrong for
        // restoring a state that was already resolved when it was saved.
        std::vector<MobEffectInstance>& MutableActiveEffects() { return m_activeEffects; }

        // The load path. Replaces the active set with `effects` verbatim —
        // durations, amplifiers and hidden chains exactly as they were saved
        // — and then re-applies each one's ATTRIBUTE MODIFIERS, which is the
        // half a direct install would silently drop.
        //
        // It has to be done here rather than at the call site because this
        // engine does not persist the attribute map (modifiers are rebuilt,
        // see the save layer): a Speed II mob restored without this reload
        // keeps its potion timer and loses its speed, and the discrepancy only
        // shows up as "mobs are slower after a reload".
        void RestoreEffects(std::vector<MobEffectInstance> effects);

        const std::vector<MobEffectInstance>& ActiveEffects() const {
            return m_activeEffects;
        }

        // MC LivingEntity.canBeAffected: undead ignore POISON and REGENERATION
        // (EntityTypeTags.IGNORES_POISON_AND_REGEN — the #undead tag).
        virtual bool CanBeAffected(const MobEffectInstance& effect) const;

        // MC LivingEntity.isInvertedHealAndHarm — harming heals the undead,
        // healing harms them (EntityTypeTags.INVERTED_HEALING_AND_HARM, also
        // the #undead tag). Overridden by the player view, whose placeholder
        // entity type would otherwise read as a zombie.
        virtual bool IsInvertedHealAndHarm() const {
            return IsUndeadEntityType(GetType());
        }

        // MC LivingEntity.canBreatheUnderwater — the CAN_BREATHE_UNDER_WATER
        // entity-type tag (#undead + the water breathers). A tag in vanilla,
        // a type predicate here, next to the undead tag it includes.
        virtual bool CanBreatheUnderwater() const;

        // MC LivingEntity.getJumpBoostPower — +0.1 per JUMP_BOOST level.
        float GetJumpBoostPower() const;

        // MC LivingEntity.getEffectiveGravity — SLOW_FALLING caps gravity at
        // 0.01 while actually falling (deltaMovement.y <= 0).
        double GetEffectiveGravity() const;

        // MC Player.causeFoodExhaustion / FoodData.eat — the player-only
        // halves of HUNGER and SATURATION. No-ops on mobs, exactly as MC's
        // `instanceof Player` guards make them; the server's player view
        // forwards both into ServerPlayer's FoodData.
        virtual void CauseFoodExhaustion(float amount) { (void)amount; }
        virtual void EatFood(int nutrition, float saturationModifier) {
            (void)nutrition; (void)saturationModifier;
        }

        // ── Health ─────────────────────────────────────────────────────────
        float GetHealth() const { return m_health; }
        void  SetHealth(float h);
        // MC LivingEntity.heal. Virtual because the server's player view has
        // to forward healing into ServerPlayer (its own health is a mirror
        // that would be overwritten next tick).
        virtual void Heal(float amount);
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

        // MC LivingEntity.causeFallDamage / calculateFallDamage:
        //   damage = floor((fd + 1e-6 - SAFE_FALL_DISTANCE) * mult * FALL_DAMAGE_MULTIPLIER)
        // SAFE_FALL_DISTANCE defaults to 3.0, so a 23-block drop deals 20 and
        // kills a full-health zombie while 22 leaves it at half a heart —
        // the numbers every drop-farm design is built on.
        bool CauseFallDamage(double fallDist, float damageMultiplier) override;
        int  CalculateFallDamage(double fallDist, float damageMultiplier) const;

        // MC LivingEntity.getMaxFallDistance -> getComfortableFallDistance(0).
        int GetMaxFallDistance() const override { return GetComfortableFallDistance(0.0f); }

        // MC's FlyingAnimal marker interface (bee, parrot, allay). Travel uses
        // air friction for the vertical axis instead of the falling 0.98.
        virtual bool IsFlyingAnimal() const { return false; }

        // MC's helper is a hardcoded floor(allowed + 3.0F) — NOT the
        // SAFE_FALL_DISTANCE attribute, oddly enough.
        int GetComfortableFallDistance(float allowedDamage) const {
            return static_cast<int>(std::floor(allowedDamage + 3.0f));
        }

        virtual void Die(MobDamageSource source, Entity* attacker);
        virtual void TickDeath();

        // ── Damage bookkeeping (read by HurtByTargetGoal) ──────────────────
        // Persisted, so it is an EntityRef rather than a raw pointer: the
        // attacker may be offline or in an unloaded chunk when this loads, and
        // vanilla keeps the identity indefinitely in that case.
        Entity* GetLastHurtByMob();                 // non-const: resolves lazily
        void    SetLastHurtByMob(Entity* e);
        void    SetLastHurtByMobUuid(const Uuid& uuid) { m_lastHurtByMobRef.SetUnresolved(uuid); }
        const EntityRef& LastHurtByMobRef() const { return m_lastHurtByMobRef; }
        int64_t GetLastHurtByMobTimestamp() const { return m_lastHurtByMobTimestamp; }
        void    SetLastHurtByMobTimestamp(int64_t t) { m_lastHurtByMobTimestamp = t; }

        // MC LivingEntity.AbsorptionAmount — a base NBT key with no field here
        // until now.
        float GetAbsorptionAmount() const { return m_absorptionAmount; }
        void  SetAbsorptionAmount(float v) { m_absorptionAmount = v < 0.0f ? 0.0f : v; }
        MobDamageSource GetLastDamageSource() const { return m_lastDamageSource; }
        // MC LivingEntity.getLastDamageSource nulls itself 40 ticks after the
        // hit (lastDamageStamp) — the memory the witch's fire-resistance drink
        // and PanicGoal read. Callers pair this with GetLastDamageSource.
        bool    HasLastDamageSource() const {
            return m_hasLastDamageSource && tickCount - m_lastDamageStamp < 40;
        }

        Entity* GetLastHurtMob() const { return m_lastHurtMob; }
        void    SetLastHurtMob(Entity* e);
        // MC LivingEntity.getLastHurtMobTimestamp — OwnerHurtTargetGoal's
        // "is this a NEW victim" test, mirroring the hurt-by pair above.
        int64_t GetLastHurtMobTimestamp() const { return m_lastHurtMobTimestamp; }

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

        // MC LivingEntity.getAttackAnim — the sub-tick lerp of the swing
        // clock, wrap-aware (the clock resets to 0 when a swing restarts
        // mid-flight). Without the lerp a 6-tick swing renders as 6 steps.
        float GetAttackAnim(float partialTick) const {
            float f = attackAnim - oAttackAnim;
            if (f < 0.0f) f += 1.0f;
            return oAttackAnim + f * partialTick;
        }

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

        // MC LivingEntity.handleEntityEvent — of its long switch, the one
        // case this port answers is 60 (POOF, sent at despawn/death removal);
        // the equipment-break and drown cases wait on their systems.
        void HandleEntityEvent(uint8_t id) override;

        // MC LivingEntity.makePoofParticles — 20 POOF particles across the
        // body, drifting outward (spawn offset −v·10 gives the puff its
        // initial spread). Client-side; addParticle no-ops on the server.
        void MakePoofParticles();

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

        // MC LivingEntity.getDamageAfterMagicAbsorb — the RESISTANCE effect's
        // 20%-per-level reduction (Void bypasses it, MC BYPASSES_RESISTANCE).
        // `attacker` is threaded through for the Witch override, which zeroes
        // self-inflicted splash damage exactly as MC's damageSource.getEntity()
        // == this check does.
        virtual float GetDamageAfterMagicAbsorb(MobDamageSource source, float amount,
                                                Entity* attacker) const;

        // MC LivingEntity.tickEffects — duration countdown, cadenced
        // applyEffectTick, hidden-effect promotion, expiry. Called at the end
        // of BaseTick, where MC's baseTick calls it; ALSO called directly by
        // the server's player view, which is never BaseTick'ed.
        void TickEffects();

        // ── Air supply / drowning (MC LivingEntity.baseTick's water block) ─
        //
        // Runs inside BaseTick, server-side: eyes underwater and unable to
        // breathe there → air ticks down; at -20 it snaps to 0 and 2.0 drown
        // damage lands, over and over until the mob surfaces or dies. Out of
        // water air recovers 4/tick. WaterAnimal (Fish/Squid) INVERTS this in
        // its own BaseTick — see Fish::HandleAirSupply.
        void HandleUnderwaterAir();

        // MC LivingEntity.decreaseAirSupply — the RESPIRATION (OXYGEN_BONUS)
        // skip roll lives in MC's version; no such attribute exists here
        // (mob armor is not modelled), so the base is a plain -1. Overridden
        // by IronGolem (never loses air).
        virtual int DecreaseAirSupply(int currentSupply);

        // MC LivingEntity.increaseAirSupply: +4/tick, clamped. The dolphin
        // overrides to refill instantly.
        virtual int IncreaseAirSupply(int currentSupply) {
            const int max = GetMaxAirSupply();
            return currentSupply + 4 < max ? currentSupply + 4 : max;
        }

        // MC LivingEntity.shouldTakeDrowningDamage.
        bool ShouldTakeDrowningDamage() const { return GetAirSupply() <= -20; }

        // MC onEffectAdded / onEffectUpdated / onEffectsRemoved — the
        // attribute-modifier bookkeeping. (The passenger/packet halves wait on
        // client sync.)
        void OnEffectAdded(const MobEffectInstance& effect);
        void OnEffectUpdated(const MobEffectInstance& effect, bool refreshAttributes);
        void OnEffectRemoved(const MobEffectInstance& effect);

        // MC LivingEntity.tickHeadTurn — Mob overrides this to run the body
        // rotation control instead.
        virtual void TickHeadTurn(float yBodyRotTarget);

        // MC LivingEntity.isPushable: alive (spectators and climbing don't
        // reach mob physics here).
        bool IsPushable() const override { return IsAlive(); }

        // MC LivingEntity.pushEntities + Entity.push(Entity) — crowd shoving
        // and the maxEntityCramming damage valve. Called from aiStep's tail.
        void PushEntities();
        void DoPush(Entity& other);

        // MC LivingEntity.serverAiStep — empty here, final on Mob.
        virtual void ServerAiStep() {}

        // MC hurtServer's `noActionTime = 0` — the counter lives on Mob in
        // this port, so the reset is a hook Mob overrides.
        virtual void ResetNoActionTime() {}

        // MC LivingEntity.updateWalkAnimation. Overridden by Chicken? No — by
        // nothing among our eight, but kept virtual to match MC.
        virtual void UpdateWalkAnimation(float distance);

        void UpdateSwingTime();
        // MC LivingEntity.getCurrentSwingDuration — 6-tick WHACK base,
        // haste/mining-fatigue adjusted. Shared by the swing clock and the
        // Swing() restart threshold.
        int  GetCurrentSwingDuration() const;

        AttributeMap m_attributes;

        // MC's activeEffects map. A flat vector keyed by linear search: the
        // busiest entity in the game holds three or four effects at once, and
        // a map node per poison tick is the wrong trade.
        std::vector<MobEffectInstance> m_activeEffects;
        MobEffectInstance* FindEffect(MobEffectId effect);

        float m_health = 20.0f;
        float m_speed  = 0.0f;

        int   m_invulnerableTime = 0;
        float m_lastHurt = 0.0f;
        int   m_noJumpDelay = 0;
        bool  m_dead = false;

        EntityRef       m_lastHurtByMobRef;
        int64_t         m_lastHurtByMobTimestamp = 0;
        float           m_absorptionAmount = 0.0f;
        Entity*         m_lastHurtMob = nullptr;
        int64_t         m_lastHurtMobTimestamp = 0;
        MobDamageSource m_lastDamageSource = MobDamageSource::Generic;
        bool            m_hasLastDamageSource = false;
        int             m_lastDamageStamp = 0;   // MC lastDamageStamp (tickCount)
    };

    // MC Block.getFriction. Exposed because the pathfinder and the mob mover
    // both need it and neither owns the table.
    float GetBlockFriction(BlockID id);

} // namespace Game
