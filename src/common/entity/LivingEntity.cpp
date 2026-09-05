// File: src/common/entity/LivingEntity.cpp
#include "common/entity/LivingEntity.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Mth.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/Blocks.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {
        // ── MC constants, verbatim ─────────────────────────────────────────
        constexpr double kMinMovementDistance = 0.003;   // LivingEntity.MIN_MOVEMENT_DISTANCE
        constexpr float  kInputFriction       = 0.98f;   // LivingEntity.INPUT_FRICTION
        constexpr float  kVerticalDrag        = 0.98f;   // travelInAir, non-flying
        constexpr float  kFrictionBase        = 0.91f;   // travelInAir
        constexpr float  kGroundAccel         = 0.21600002f;
        constexpr float  kFlyingSpeed         = 0.02f;   // getFlyingSpeed, unridden
        constexpr float  kDefaultKnockback    = 0.4f;
        constexpr int    kInvulnerableDuration = 20;
        constexpr int    kHurtDuration        = 10;
        constexpr int    kDeathDuration       = 20;
        constexpr double kFluidJumpImpulse    = 0.04;
        constexpr int    kJumpDelay           = 10;
    }

    float GetBlockFriction(BlockID id) {
        // MC Blocks.java: the default is 0.6 and only a handful override it.
        switch (id) {
            case BlockID::Ice:
            case BlockID::PackedIce:
                return 0.98f;
            case BlockID::BlueIce:
                return 0.989f;
            case BlockID::SlimeBlock:
                return 0.8f;
            default:
                return 0.6f;
        }
    }

    LivingEntity::LivingEntity(EntityTypeId type, EntityLevel* level)
        : Entity(type, level) {
        CreateLivingAttributes(m_attributes);
        m_health = GetMaxHealth();
    }

    LivingEntity::LivingEntity(EntityTypeId type, EntityLevel* level, NoAttributesTag)
        : Entity(type, level) {
        // No CreateLivingAttributes — see the header. GetMaxHealth still reads
        // 20.0 here, out of kAttributeTable rather than out of a registered
        // instance, so the starting health is the same number by the same rule.
        m_health = GetMaxHealth();
    }

    LivingEntity::~LivingEntity() = default;

    void LivingEntity::SetHealth(float h) {
        m_health = std::clamp(h, 0.0f, GetMaxHealth());
    }

    void LivingEntity::Heal(float amount) {
        // MC LivingEntity.heal: only the living are healed; setHealth clamps.
        if (GetHealth() <= 0.0f) return;
        SetHealth(GetHealth() + amount);
    }

    // ── Status effects ─────────────────────────────────────────────────────

    MobEffectInstance* LivingEntity::FindEffect(MobEffectId effect) {
        for (MobEffectInstance& e : m_activeEffects) {
            if (e.effect == effect) return &e;
        }
        return nullptr;
    }

    const MobEffectInstance* LivingEntity::GetEffect(MobEffectId effect) const {
        for (const MobEffectInstance& e : m_activeEffects) {
            if (e.effect == effect) return &e;
        }
        return nullptr;
    }

    bool LivingEntity::CanBeAffected(const MobEffectInstance& effect) const {
        // MC LivingEntity.canBeAffected: the IGNORES_POISON_AND_REGEN tag is
        // the #undead set — the same membership as inverted heal-and-harm.
        // (The INFESTED/OOZING immunities guard effects this port has none of.)
        if (!IsInvertedHealAndHarm()) return true;
        return effect.effect != MobEffectId::Regeneration &&
               effect.effect != MobEffectId::Poison;
    }

    bool LivingEntity::AddEffect(MobEffectInstance effect, Entity* source) {
        // MC LivingEntity.addEffect(newEffect, source). `source` is kept for
        // signature parity — MC uses it for packet attribution only.
        (void)source;
        if (!CanBeAffected(effect)) return false;

        MobEffectInstance* existing = FindEffect(effect.effect);
        bool changed = false;
        if (!existing) {
            m_activeEffects.push_back(std::move(effect));
            OnEffectAdded(m_activeEffects.back());
            changed = true;
        } else if (existing->Update(effect)) {
            OnEffectUpdated(*existing, /*refreshAttributes=*/true);
            changed = true;
        }
        // MC also runs MobEffect.onEffectStarted / onEffectAdded here — sound
        // and BadOmen machinery; nothing in this effect set implements either.
        return changed;
    }

    bool LivingEntity::RemoveEffect(MobEffectId effect) {
        for (size_t i = 0; i < m_activeEffects.size(); ++i) {
            if (m_activeEffects[i].effect != effect) continue;
            MobEffectInstance removed = std::move(m_activeEffects[i]);
            m_activeEffects.erase(m_activeEffects.begin() +
                                  static_cast<ptrdiff_t>(i));
            OnEffectRemoved(removed);
            return true;
        }
        return false;
    }

    bool LivingEntity::RemoveAllEffects() {
        if (m_activeEffects.empty()) return false;
        std::vector<MobEffectInstance> removed = std::move(m_activeEffects);
        m_activeEffects.clear();
        for (const MobEffectInstance& e : removed) OnEffectRemoved(e);
        return true;
    }

    void LivingEntity::RestoreEffects(std::vector<MobEffectInstance> effects) {
        // Drop the modifiers the old set installed before replacing it, so a
        // reload onto an entity that already had effects cannot stack them.
        for (const auto& e : m_activeEffects) {
            RemoveEffectAttributeModifiers(m_attributes, e.effect);
        }
        m_activeEffects = std::move(effects);
        for (const auto& e : m_activeEffects) {
            AddEffectAttributeModifiers(m_attributes, e.effect, e.amplifier);
        }
    }

    void LivingEntity::OnEffectAdded(const MobEffectInstance& effect) {
        AddEffectAttributeModifiers(m_attributes, effect.effect, effect.amplifier);
        // Client sync (ClientboundUpdateMobEffectPacket) is the documented
        // follow-up; nothing renders an effect yet.
    }

    void LivingEntity::OnEffectUpdated(const MobEffectInstance& effect,
                                       bool refreshAttributes) {
        if (refreshAttributes) {
            RemoveEffectAttributeModifiers(m_attributes, effect.effect);
            AddEffectAttributeModifiers(m_attributes, effect.effect, effect.amplifier);
        }
    }

    void LivingEntity::OnEffectRemoved(const MobEffectInstance& effect) {
        RemoveEffectAttributeModifiers(m_attributes, effect.effect);
    }

    void LivingEntity::TickEffects() {
        // Server-only: the client's mob copies never hold an effect (no sync
        // exists), and MC's client branch is particle spawning anyway.
        if (!m_level || m_level->IsClientSide()) return;

        for (size_t i = 0; i < m_activeEffects.size();) {
            bool downgraded = false;
            if (!m_activeEffects[i].TickServer(*this, downgraded)) {
                MobEffectInstance removed = std::move(m_activeEffects[i]);
                m_activeEffects.erase(m_activeEffects.begin() +
                                      static_cast<ptrdiff_t>(i));
                OnEffectRemoved(removed);
            } else {
                // MC's onEffectUpdate runnable: a hidden effect surfacing must
                // re-fold the attribute modifiers at the new amplifier.
                if (downgraded) OnEffectUpdated(m_activeEffects[i], true);
                ++i;
            }
        }
        // Death does NOT clear effects: MC clears them only through a player's
        // respawn, and a dying mob is removed with its map intact.
    }

    float LivingEntity::GetJumpBoostPower() const {
        // MC LivingEntity.getJumpBoostPower.
        const MobEffectInstance* jump = GetEffect(MobEffectId::JumpBoost);
        return jump ? 0.1f * static_cast<float>(jump->amplifier + 1) : 0.0f;
    }

    double LivingEntity::GetEffectiveGravity() const {
        // MC LivingEntity.getEffectiveGravity: SLOW_FALLING only helps while
        // actually falling — a rising entity keeps full gravity.
        const bool isFalling = velocity.y <= 0.0;
        if (isFalling && HasEffect(MobEffectId::SlowFalling)) {
            return std::min(GetGravity(), 0.01);
        }
        return GetGravity();
    }

    bool LivingEntity::IsEffectiveAi() const {
        return m_level && !m_level->IsClientSide();
    }

    void LivingEntity::SetLastHurtByMob(Entity* e) {
        if (e) MarkHoldsEntityRefs();   // see Entity::HoldsEntityRefs
        m_lastHurtByMobRef.Set(e);
        m_lastHurtByMobTimestamp = m_level ? m_level->GetGameTime() : 0;
    }

    void LivingEntity::SetLastHurtMob(Entity* e) {
        // MC setLastHurtMob stamps lastHurtMobTimestamp = tickCount; game time
        // is this port's shared clock for the hurt-by pair, so it is used for
        // both — the goals only compare for inequality.
        if (e) MarkHoldsEntityRefs();   // see Entity::HoldsEntityRefs
        m_lastHurtMob = e;
        m_lastHurtMobTimestamp = m_level ? m_level->GetGameTime() : 0;
    }

    void LivingEntity::ClearReferenceTo(const Entity* entity) {
        // Demote the pointer but keep the IDENTITY when the attacker merely
        // unloaded — it may come back, and a saved grudge outlives a chunk
        // boundary. OnEntityRemoved drops the uuid only for a real death.
        m_lastHurtByMobRef.OnEntityRemoved(entity);
        // Not persisted, so a plain null is right here.
        if (m_lastHurtMob == entity) m_lastHurtMob = nullptr;
    }

    Entity* LivingEntity::GetLastHurtByMob() {
        return m_level ? m_lastHurtByMobRef.Get(*m_level) : nullptr;
    }

    void LivingEntity::Swing() {
        // MC LivingEntity.swing — restart the arm swing unless one is already
        // more than half done (getCurrentSwingDuration() / 2, so haste and
        // mining fatigue move the restart point too), so rapid attacks do not
        // look frozen.
        if (!swinging || swingTime >= GetCurrentSwingDuration() / 2 || swingTime < 0) {
            swingTime = -1;
            swinging = true;
            // MC broadcasts ClientboundAnimatePacket(SWING_MAIN_HAND) from
            // the server here — without it no client ever sees a mob's melee
            // whack. This port's stand-in is a custom entity event; the
            // client handler calls Swing() back on its copy.
            if (m_level && !m_level->IsClientSide()) {
                m_level->BroadcastEntityEvent(*this, kEntityEventSwing);
            }
        }
    }

    int LivingEntity::GetCurrentSwingDuration() const {
        // MC LivingEntity.getCurrentSwingDuration: the empty-hand base is 6
        // ticks (SwingAnimationType.WHACK), HASTE takes (1 + amp) off, MINING
        // FATIGUE adds (1 + amp) * 2 — the visibly sluggish arm an elder
        // guardian's aura gives everything it touches.
        int swingDuration = 6;
        if (const MobEffectInstance* haste = GetEffect(MobEffectId::Haste)) {
            swingDuration -= 1 + haste->amplifier;
        } else if (const MobEffectInstance* fatigue =
                       GetEffect(MobEffectId::MiningFatigue)) {
            swingDuration += (1 + fatigue->amplifier) * 2;
        }
        // Guard MC does not need: Haste VI+ would zero the duration and the
        // attackAnim division in UpdateSwingTime. Unreachable — nothing
        // grants haste past beacon II — but a division by zero is not worth
        // the parity point.
        return swingDuration < 1 ? 1 : swingDuration;
    }

    void LivingEntity::UpdateSwingTime() {
        const int swingDuration = GetCurrentSwingDuration();

        if (swinging) {
            ++swingTime;
            if (swingTime >= swingDuration) {
                swingTime = 0;
                swinging = false;
            }
        } else {
            swingTime = 0;
        }
        attackAnim = static_cast<float>(swingTime) / static_cast<float>(swingDuration);
    }

    float LivingEntity::GetJumpPower() const {
        // MC LivingEntity.getJumpPower: JUMP_STRENGTH * blockJumpFactor +
        // getJumpBoostPower(). No block has a jump factor in this engine
        // (honey/slime jump damping is not modelled), so the factor is 1.
        return static_cast<float>(GetAttributeValue(Attribute::JumpStrength)) +
               GetJumpBoostPower();
    }

    void LivingEntity::JumpFromGround() {
        const float jumpPower = GetJumpPower();
        if (jumpPower <= 1.0e-5f) return;

        // MC takes the MAX of the jump impulse and current upward motion, so a
        // mob already rising (stepped up, bounced) is not slowed by jumping.
        velocity.y = std::max(static_cast<double>(jumpPower), velocity.y);

        if (IsSprinting()) {
            const float angle = yRot * Mth::kDegToRad;
            velocity.x += -std::sin(angle) * 0.2;
            velocity.z +=  std::cos(angle) * 0.2;
        }

        needsSync = true;
    }

    void LivingEntity::Travel(const glm::dvec3& input) {
        // MC LivingEntity.travel dispatches to travelInFluid / travelFallFlying
        // / travelInAir. Fluid travel is folded into the air path below rather
        // than ported separately: this engine has no fluid height or flow, so
        // MC's water branch (which is built entirely on getFluidHeight and
        // getFluidFallingAdjustedMovement) has no inputs to read. What IS
        // modelled is the part players notice — swimming mobs sink slowly and
        // move at reduced speed — via the drag values below.
        const bool inFluid = IsInWater() || IsInLava();

        // MC Entity.getBlockPosBelowThatAffectsMyMovement -> getOnPos(0.500001F)
        // — half a block down (plus an epsilon so an exact block boundary
        // rounds DOWN), not 0.2 (that is getOnPosLegacy). The difference is
        // which block's slipperiness a mob on a slab above ice reads.
        const int belowY = static_cast<int>(std::floor(position.y - 0.500001));
        const int belowX = static_cast<int>(std::floor(position.x));
        const int belowZ = static_cast<int>(std::floor(position.z));

        float blockFriction = 1.0f;
        if (onGround && m_level && m_level->Blocks()) {
            blockFriction = GetBlockFriction(m_level->Blocks()->GetBlock(belowX, belowY, belowZ));
            // MC 26.3: computeModifiedFriction(friction, FRICTION_MODIFIER) —
            // the modifier scales how far the block is from frictionless,
            // 1.0 (every mob but the sulfur cube's archetypes) is identity.
            blockFriction = ComputeModifiedFriction(
                blockFriction, static_cast<float>(GetAttributeValue(Attribute::FrictionModifier)));
        }

        if (inFluid) {
            const double oldY = position.y;
            if (IsInLava() && !IsInWater()) {
                // MC travelInLava: 0.02 acceleration, horizontal drag 0.5,
                // vertical 0.8 (the shallow branch — with no fluid-height
                // model the ≤0.4-deep constants stand in for both; the deep
                // branch differs only in vertical drag 0.5), then a QUARTER
                // gravity — lava is four times as sticky downward as water.
                MoveRelative(0.02f, input);
                Move(velocity);
                velocity.x *= 0.5;
                velocity.z *= 0.5;
                velocity.y *= 0.8;
                if (!IsCreative() && !IsNoGravity()) velocity.y -= GetEffectiveGravity() / 4.0;
            } else {
                // MC travelInWater: horizontal drag 0.8 (0.9 sprinting), a fixed
                // 0.02 acceleration, and gravity reduced to a sixteenth —
                // gated on !isSprinting(), so a sprint-swimming mob does not
                // sink at all.
                const float waterDrag = IsSprinting() ? 0.9f : 0.8f;
                MoveRelative(0.02f, input);
                Move(velocity);
                velocity.x *= waterDrag;
                velocity.z *= waterDrag;
                velocity.y *= 0.8;
                // MC travelInFluid also reads getEffectiveGravity(), so SLOW_FALLING
                // slows a sinking mob in water exactly as it does in air.
                if (!IsSprinting() && !IsCreative() && !IsNoGravity()) {
                    velocity.y -= GetEffectiveGravity() / 16.0;
                }
            }
            // MC jumpOutOfFluid, called from both fluid travels: a swimming
            // mob pressed against a bank (horizontalCollision) whose body
            // would fit 0.6 above pops out with vy=0.3 — without it nothing
            // can climb out of water at an edge. MC Entity.isFree = the moved
            // box clears blocks AND holds no liquid — the liquid half is what
            // stops a mob against an underwater wall from riding 0.3 all the
            // way up it.
            const auto movedBoxIsFree = [&](double dx, double dy, double dz) {
                AABB box = GetAABB();
                const glm::vec3 off(static_cast<float>(dx),
                                    static_cast<float>(dy),
                                    static_cast<float>(dz));
                box.min += off;
                box.max += off;
                if (CollidesAt(box, m_level->Physics())) return false;
                const IBlockAccess* blocks = m_level->Blocks();
                if (!blocks) return true;
                for (int by = static_cast<int>(std::floor(box.min.y));
                     by <= static_cast<int>(std::floor(box.max.y)); ++by)
                    for (int bx = static_cast<int>(std::floor(box.min.x));
                         bx <= static_cast<int>(std::floor(box.max.x)); ++bx)
                        for (int bz = static_cast<int>(std::floor(box.min.z));
                             bz <= static_cast<int>(std::floor(box.max.z)); ++bz)
                            if (blocks->IsBlockFluid(bx, by, bz)) return false;
                return true;
            };
            if (horizontalCollision &&
                movedBoxIsFree(velocity.x, velocity.y + 0.6 - position.y + oldY,
                               velocity.z)) {
                velocity.y = 0.3;
            }
            return;
        }

        // MC 26.3 travelInAir: airDrag = computeModifiedFriction(0.91,
        // AIR_DRAG_MODIFIER), the vertical drag the same fold over 0.98 — or
        // airDrag itself for an omnidirectional air mover (and, as before,
        // for a flying animal). Modifier 1.0 reproduces the old constants.
        const float airDragModifier =
            static_cast<float>(GetAttributeValue(Attribute::AirDragModifier));
        const float airDrag  = ComputeModifiedFriction(kFrictionBase, airDragModifier);
        const float friction = blockFriction * airDrag;

        // MC handleRelativeFrictionAndCalculateMovement.
        const float accel = onGround
            ? m_speed * (kGroundAccel / (blockFriction * blockFriction * blockFriction))
            : kFlyingSpeed;
        MoveRelative(accel, input);
        const glm::dvec3 preMove = velocity;
        const glm::dvec3 prePos  = position;
        Move(velocity);
        RestituteMovementAfterCollisions(preMove, position - prePos, airDrag);

        // MC travelInAir's vertical term: LEVITATION replaces gravity outright,
        // easing the motion toward 0.05 * (amp + 1) at 20% per tick — which is
        // why a levitating mob rises smoothly instead of snapping. Otherwise
        // gravity applies, capped at 0.01 by SLOW_FALLING while falling.
        double movementY;
        if (const MobEffectInstance* lev = GetEffect(MobEffectId::Levitation)) {
            movementY = velocity.y +
                        (0.05 * static_cast<double>(lev->amplifier + 1) - velocity.y) * 0.2;
        } else {
            movementY = velocity.y - (IsNoGravity() ? 0.0 : GetEffectiveGravity());
        }

        // MC applies horizontal friction and vertical drag AFTER the move, so
        // the drag scales what actually happened rather than what was asked
        // for. Doing it before scales a value the collision is about to throw
        // away — the same ordering trap called out in ItemEntity::TickMovement.
        if (m_discardFriction) {
            // MC travelInAir's shouldDiscardFriction branch: the movement is
            // taken as-is. Skipping BOTH terms is the point — damping just the
            // horizontal would still bleed the arc's height away.
            velocity.y = movementY;
        } else {
            // MC: a FlyingAnimal's vertical drag is the same air friction as
            // the horizontal axes (0.91-based), not the falling body's 0.98 —
            // it is what lets a bee hold altitude instead of slowly sinking.
            const float verticalFriction = (IsFlyingAnimal() || OmnidirectionalAirMover())
                ? friction
                : ComputeModifiedFriction(kVerticalDrag, airDragModifier);
            velocity.x *= friction;
            velocity.y  = movementY * verticalFriction;
            velocity.z *= friction;
        }
    }

    float LivingEntity::ComputeModifiedFriction(float friction, float modifier) {
        // MC 26.3 LivingEntity.computeModifiedFriction.
        return std::clamp(1.0f - (1.0f - friction) * modifier, 0.0f, 1.0f);
    }

    double LivingEntity::GetEntityBounciness() const {
        return GetAttributeValue(Attribute::Bounciness);
    }

    void LivingEntity::RestituteMovementAfterCollisions(const glm::dvec3& preMove,
                                                        const glm::dvec3& moved,
                                                        float airDrag) {
        // MC 26.3 Entity.restituteMovementAfterCollisions, the ENTITY-bounciness
        // half: a collided axis keeps -v * restitution instead of the zero
        // Move() left there. (MC's other half, the block's own restitution —
        // the slime block — lives with the block code here, as before.)
        // `preMove` is MC's currentMovement (deltaMovement before the move),
        // `moved` the movement that actually happened. Nothing to do for the
        // whole world but a sulfur cube wearing a block: bounciness is 0.
        const double restitution = GetEntityBounciness();
        if (restitution <= 0.0) return;
        const bool xCollision = preMove.x != 0.0 && velocity.x == 0.0;
        const bool zCollision = preMove.z != 0.0 && velocity.z == 0.0;
        if (!verticalCollision && !xCollision && !zCollision) return;

        glm::dvec3 after = velocity;
        if (xCollision) after.x = -preMove.x * restitution;
        if (zCollision) after.z = -preMove.z * restitution;

        if (verticalCollision) {
            // verticalCollisionBelow: a landing keeps its bounce only when it
            // came in faster than one tick of gravity — a resting cube does
            // not vibrate on the floor.
            double r = restitution;
            if (preMove.y < 0.0 && !(-preMove.y > GetEffectiveGravity())) r = 0.0;
            double gravityCompensation = 0.0;
            double effectiveDrag = 1.0;
            if (r > 0.0 && preMove.y != 0.0) {
                const double portionWithMovement = moved.y / preMove.y;
                gravityCompensation = portionWithMovement * GetEffectiveGravity();
                effectiveDrag = 1.0 + (static_cast<double>(airDrag) - 1.0) * portionWithMovement;
            }
            after.y = (gravityCompensation - preMove.y) * effectiveDrag * r;
        }
        velocity = after;
        needsSync = true;
    }

    int LivingEntity::CalculateFallDamage(double fallDist, float damageMultiplier) const {
        // MC LivingEntity.calculateFallDamage. The +1e-6 makes an exact
        // 3.0-block fall deal 0 rather than flapping on float error.
        const double fallPower =
            fallDist + 1.0e-6 - GetAttributeValue(Attribute::SafeFallDistance);
        return static_cast<int>(std::floor(
            fallPower * damageMultiplier * GetAttributeValue(Attribute::FallDamageMultiplier)));
    }

    bool LivingEntity::CauseFallDamage(double fallDist, float damageMultiplier) {
        // hurt() is server business — the client's copy runs Travel too, and
        // damaging there would desync health.
        if (m_level && m_level->IsClientSide()) return false;

        const int damage = CalculateFallDamage(fallDist, damageMultiplier);
        if (damage <= 0) return false;
        Hurt(MobDamageSource::Fall, static_cast<float>(damage), nullptr);
        return true;
    }

    void LivingEntity::UpdateWalkAnimation(float distance) {
        const float targetSpeed = std::min(distance * 4.0f, 1.0f);
        walkAnimation.Update(targetSpeed, 0.4f, IsBaby() ? 3.0f : 1.0f);
    }

    // MC LivingEntity.aiStep calls calculateEntityAnimation(this instanceof
    // FlyingAnimal) — vertical distance counts toward the walk cycle for the
    // parrot/bee/allay family, so a climbing bee's wings pace with it.
    void LivingEntity::CalculateEntityAnimation(bool useY) {
        const float distance = static_cast<float>(Mth::Length(
            position.x - oldPosition.x,
            useY ? position.y - oldPosition.y : 0.0,
            position.z - oldPosition.z));
        if (IsAlive()) UpdateWalkAnimation(distance);
        else walkAnimation.Stop();
    }

    void LivingEntity::AiStep() {
        if (m_noJumpDelay > 0) --m_noJumpDelay;

        // ── Motion deadzone (MC LivingEntity.aiStep) ───────────────────────
        // Non-players zero each axis independently below 0.003. Without this a
        // mob standing on flat ground keeps a residual sub-millimetre drift
        // forever, which shows up as a permanently-running walk animation.
        if (std::abs(velocity.x) < kMinMovementDistance) velocity.x = 0.0;
        if (std::abs(velocity.y) < kMinMovementDistance) velocity.y = 0.0;
        if (std::abs(velocity.z) < kMinMovementDistance) velocity.z = 0.0;

        // MC LivingEntity.aiStep: applyInput() — the 0.98 steering decay —
        // runs BEFORE the immobile/serverAiStep branch, so the values
        // MoveControl writes THIS tick reach travel undecayed. Decaying after
        // ServerAiStep (as this used to) shaved every AI mob's speed to 98%
        // of vanilla.
        xxa *= kInputFriction;
        zza *= kInputFriction;

        if (IsImmobile()) {
            jumping = false;
            xxa = 0.0f;
            zza = 0.0f;
        } else if (IsEffectiveAi()) {
            ServerAiStep();
        }

        // ── Jump ───────────────────────────────────────────────────────────
        if (jumping) {
            if (IsInLiquid()) {
                velocity.y += kFluidJumpImpulse;
            } else if (onGround && m_noJumpDelay == 0) {
                JumpFromGround();
                m_noJumpDelay = kJumpDelay;
            }
        } else {
            m_noJumpDelay = 0;
        }

        // MC aiStep, right before travel: SLOW_FALLING and LEVITATION reset
        // the fall distance every tick they are active — which is the whole
        // fall-damage negation, not a special case in calculateFallDamage.
        if (HasEffect(MobEffectId::SlowFalling) || HasEffect(MobEffectId::Levitation)) {
            ResetFallDistance();
        }

        // ── Travel ─────────────────────────────────────────────────────────
        const glm::dvec3 input(xxa, yya, zza);
        if (IsEffectiveAi() || (m_level && m_level->IsClientSide())) {
            Travel(input);
        }

        if (m_level && m_level->IsClientSide()) {
            CalculateEntityAnimation(IsFlyingAnimal());
        }

        // MC aiStep's tail: pushEntities() — crowd shoving. Without it mobs
        // stack inside one another and a farm's crowd never spreads into the
        // drop chute. Server-side only here: a client mob's position is the
        // server's to dictate.
        if (IsEffectiveAi()) {
            PushEntities();
        }
    }

    void LivingEntity::PushEntities() {
        if (!m_level) return;
        std::vector<Entity*> list;
        m_level->GetEntitiesInBox(GetAABB(), this, list);
        // MC Level.getPushableEntities = EntitySelector.pushableBy(this):
        // pushable and alive (team no-push rules are scoreboard machinery,
        // absent).
        std::erase_if(list, [](Entity* e) { return !e->IsPushable(); });
        if (list.empty()) return;

        // MC MAX_ENTITY_CRAMMING (default 24): with more than 23 pushable
        // non-passenger neighbours, 6.0 cramming damage on a 1-in-4 roll per
        // tick — the overcrowding valve cramming farms are built on.
        constexpr int kMaxEntityCramming = 24;
        if (static_cast<int>(list.size()) > kMaxEntityCramming - 1 &&
            m_level->Random().NextInt(4) == 0) {
            int count = 0;
            for (Entity* e : list) {
                if (!e->IsPassenger()) ++count;
            }
            if (count > kMaxEntityCramming - 1) {
                Hurt(MobDamageSource::Cramming, 6.0f, nullptr);
            }
        }

        for (Entity* e : list) {
            DoPush(*e);
        }
    }

    void LivingEntity::DoPush(Entity& other) {
        // MC Entity.push(Entity), verbatim: skip co-passengers of one
        // vehicle, take the LARGER horizontal axis as the distance measure
        // (absMax), fall off with sqrt, cap the impulse, scale by 0.05, and
        // shove BOTH parties half each — unless one is a vehicle or
        // unpushable. (Pushing a player view moves nothing — player motion
        // is client-authoritative — but the mob's half still applies, which
        // is how a player wades through a crowd.)
        const auto rootVehicle = [](Entity* e) {
            while (e->GetVehicle()) e = e->GetVehicle();
            return e;
        };
        if (rootVehicle(&other) == rootVehicle(this)) return;

        double xa = other.position.x - position.x;
        double za = other.position.z - position.z;
        double dd = std::max(std::abs(xa), std::abs(za));
        if (dd < 0.009999999776482582) return;   // MC (double)0.01F

        dd = std::sqrt(dd);
        xa /= dd;
        za /= dd;
        double pow = 1.0 / dd;
        if (pow > 1.0) pow = 1.0;
        xa *= pow * 0.05;
        za *= pow * 0.05;

        if (!IsVehicle() && IsPushable()) {
            velocity.x -= xa;
            velocity.z -= za;
            needsSync = true;
        }
        if (!other.IsVehicle() && other.IsPushable()) {
            other.velocity.x += xa;
            other.velocity.z += za;
            other.needsSync = true;
        }
    }

    bool LivingEntity::CanBreatheUnderwater() const {
        // MC: EntityTypeTags.CAN_BREATHE_UNDER_WATER (see the tag helper for
        // the membership; #undead is a sub-tag).
        return CanBreatheUnderWaterEntityType(GetType());
    }

    int LivingEntity::DecreaseAirSupply(int currentSupply) {
        // MC decreaseAirSupply rolls OXYGEN_BONUS (the RESPIRATION enchant's
        // attribute) to sometimes skip the tick; no mob wears enchanted
        // helmets here, so MC's oxygenBonus == 0 path is the whole function.
        return currentSupply - 1;
    }

    void LivingEntity::HandleUnderwaterAir() {
        // MC LivingEntity.baseTick, the isEyeInFluid(WATER) block. Bubble
        // columns (which exempt the eye block) do not exist in this engine.
        if (IsEyeInWater()) {
            // MC MobEffectUtil.hasWaterBreathing: WATER_BREATHING or
            // CONDUIT_POWER (no conduits here) or BREATH_OF_THE_NAUTILUS
            // (no nautilus trinket system).
            const bool canDrownInWater =
                !CanBreatheUnderwater() && !HasEffect(MobEffectId::WaterBreathing);
            if (canDrownInWater) {
                SetAirSupply(DecreaseAirSupply(GetAirSupply()));
                if (ShouldTakeDrowningDamage()) {
                    SetAirSupply(0);
                    // MC broadcasts entity event 67 here — the client's drown
                    // bubble particles; no particle system to land them in.
                    Hurt(MobDamageSource::Drown, 2.0f, nullptr);
                }
            } else if (GetAirSupply() < GetMaxAirSupply()) {
                // MC's shouldEffectsRefillAirsupply gate is about the
                // nautilus-breath effect only; without it this is
                // unconditional, as in vanilla.
                SetAirSupply(IncreaseAirSupply(GetAirSupply()));
            }
            // MC: dismount from a vehicle that dismountsUnderwater() — no
            // rideable vehicle here answers true (boats do; none exist).
        } else if (GetAirSupply() < GetMaxAirSupply()) {
            SetAirSupply(IncreaseAirSupply(GetAirSupply()));
        }
    }

    void LivingEntity::TickCombatTimers() {
        if (hurtTime > 0) --hurtTime;
        if (m_invulnerableTime > 0) --m_invulnerableTime;

        // MC clears the "who hurt me" memory after 100 ticks, which is what
        // makes a mob eventually forget an attacker it never reached.
        // Clearing the REFERENCE, not just the cached pointer: the memory
        // has genuinely expired, so the identity should not survive a save
        // either.
        if (!m_lastHurtByMobRef.Empty() && m_level &&
            m_level->GetGameTime() - m_lastHurtByMobTimestamp > 100) {
            m_lastHurtByMobRef.Clear();
        }
    }

    void LivingEntity::BaseTick() {
        oAttackAnim = attackAnim;

        Entity::BaseTick();

        // MC baseTick's water block runs before the hurtTime countdown, on
        // the server only, for a living entity. The player halves of MC's
        // block (world border, ability-invulnerable exemption) do not apply:
        // the server's player views are never BaseTick'ed — see
        // PlayerEntityView::TickCombatState.
        if (IsAlive() && IsEffectiveAi()) {
            HandleUnderwaterAir();
        }

        TickCombatTimers();

        // Fire damage: 1.0 every 20 ticks while burning. Fire-immune types
        // (blaze, zombified piglin) shed the ticks without the damage.
        if (m_remainingFireTicks > 0 && IsEffectiveAi() && tickCount % 20 == 0) {
            if (FireImmune()) {
                m_remainingFireTicks = 0;
            } else {
                Hurt(MobDamageSource::Fire, 1.0f, nullptr);
            }
        }

        // MC baseTick ends with tickEffects(). Placement after the fire tick
        // matters for FIRE_RESISTANCE: the immunity is checked in Hurt, so the
        // order here only decides which tick a fresh fire-resistance first
        // applies on — MC's order is kept.
        TickEffects();

        if (IsDeadOrDying()) {
            TickDeath();
        }
    }

    void LivingEntity::Tick() {
        BaseTick();

        UpdateSwingTime();

        if (!IsRemoved()) {
            AiStep();
        }

        // ── Body yaw follows the direction of travel (MC LivingEntity.tick) ─
        const double xd = position.x - oldPosition.x;
        const double zd = position.z - oldPosition.z;
        const float sideDist = static_cast<float>(xd * xd + zd * zd);
        float yBodyRotTarget = yBodyRot;

        if (sideDist > 0.0025000002f) {
            const float walkDirection =
                static_cast<float>(std::atan2(zd, xd)) * Mth::kRadToDeg - 90.0f;
            const float diff = std::abs(Mth::WrapDegrees(yRot) - walkDirection);
            // Walking backwards: the body faces the way it is going, flipped,
            // rather than spinning 180 degrees.
            yBodyRotTarget = (95.0f < diff && diff < 265.0f) ? walkDirection - 180.0f
                                                             : walkDirection;
        }

        if (attackAnim > 0.0f) yBodyRotTarget = yRot;

        TickHeadTurn(yBodyRotTarget);

        // MC's "rangeChecks" block: keep each *O partner within 180 degrees of
        // its current value so render interpolation never takes the long way
        // round at the +-180 seam.
        const auto unwrap = [](float current, float& previous) {
            while (current - previous < -180.0f) previous -= 360.0f;
            while (current - previous >= 180.0f) previous += 360.0f;
        };
        unwrap(yRot, yRotO);
        unwrap(yBodyRot, yBodyRotO);
        unwrap(xRot, xRotO);
        unwrap(yHeadRot, yHeadRotO);
    }

    void LivingEntity::TickHeadTurn(float yBodyRotTarget) {
        // MC's LivingEntity default: rotate the body toward the travel
        // direction at up to 75 degrees per tick. Mob replaces this with
        // BodyRotationControl, which is far less twitchy.
        yBodyRot = Mth::RotateIfNecessary(yBodyRot, yBodyRotTarget, 75.0f);
    }

    // ── Damage ─────────────────────────────────────────────────────────────

    float LivingEntity::GetDamageAfterArmorAbsorb(MobDamageSource source, float amount) const {
        // MC CombatRules.getDamageAfterAbsorb. Armor toughness is in the
        // formula but is 0 for every mob here; keeping it spelled out means a
        // mob that gains armour later needs no change.
        const float armor = static_cast<float>(GetAttributeValue(Attribute::Armor));
        if (armor <= 0.0f) return amount;

        const float toughness = static_cast<float>(GetAttributeValue(Attribute::ArmorToughness));
        const float f = 2.0f + toughness / 4.0f;
        const float g = std::clamp(armor - amount / f, armor * 0.2f, 20.0f);
        return amount * (1.0f - g / 25.0f);
    }

    float LivingEntity::GetDamageAfterMagicAbsorb(MobDamageSource source, float amount,
                                                  Entity* attacker) const {
        (void)attacker;   // consumed by the Witch override
        // MC CombatRules via LivingEntity.getDamageAfterMagicAbsorb: the
        // RESISTANCE effect shaves 20% per level; the void bypasses it
        // (DamageTypeTags.BYPASSES_RESISTANCE).
        if (source != MobDamageSource::Void) {
            if (const MobEffectInstance* res = GetEffect(MobEffectId::Resistance)) {
                const float absorb = static_cast<float>((res->amplifier + 1) * 5);
                amount = std::max(amount * (25.0f - absorb) / 25.0f, 0.0f);
            }
        }
        return amount;
    }

    void LivingEntity::ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC: magic and wither damage carry BYPASSES_ARMOR — poison ticking
        // through a zombie's 2 armor is the observable difference.
        const bool bypassesArmor =
            source == MobDamageSource::Magic || source == MobDamageSource::Wither;
        if (!bypassesArmor) amount = GetDamageAfterArmorAbsorb(source, amount);
        amount = GetDamageAfterMagicAbsorb(source, amount, attacker);
        if (amount <= 0.0f) return;
        SetHealth(m_health - amount);
    }

    bool LivingEntity::HurtFrom(MobDamageSource source, float amount, Entity* causingEntity,
                                Entity* directEntity) {
        Entity* const previous = m_hurtDirectEntity;
        m_hurtDirectEntity = directEntity;
        const bool hit = Hurt(source, amount, causingEntity);
        m_hurtDirectEntity = previous;
        return hit;
    }

    bool LivingEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (IsRemoved() || IsDeadOrDying()) return false;
        if (amount < 0.0f) amount = 0.0f;

        // MC LivingEntity.hurtServer step 1 (isInvulnerableTo): FIRE_RESISTANCE
        // blanks every IS_FIRE source outright — no i-frames consumed, no
        // knockback, no hurt flash.
        if (source == MobDamageSource::Fire &&
            HasEffect(MobEffectId::FireResistance)) {
            return false;
        }

        // MC hurtServer: `this.noActionTime = 0;` on every accepted hit — a
        // mob that gets punched is no longer idle for the soft-despawn clock
        // and the noActionTime-gated strolls. (The counter lives on Mob in
        // this port; the hook is a no-op for other LivingEntities, as MC's
        // reset effectively is.)
        ResetNoActionTime();

        bool tookFullDamage = true;

        // ── The two-stage invulnerability window (MC hurtServer step 5) ────
        //
        // Inside the window a new hit only lands if it is STRONGER than the one
        // that opened it, and then only for the difference. This is what stops
        // a crowd of zombies from stacking their damage, and it is the rule
        // most reimplementations drop.
        if (m_invulnerableTime > kHurtDuration) {
            if (amount <= m_lastHurt) return false;
            ActuallyHurt(source, amount - m_lastHurt, attacker);
            m_lastHurt = amount;
            tookFullDamage = false;
        } else {
            m_lastHurt = amount;
            m_invulnerableTime = kInvulnerableDuration;
            ActuallyHurt(source, amount, attacker);
            hurtDuration = kHurtDuration;
            hurtTime = hurtDuration;
        }

        m_lastDamageSource = source;
        m_hasLastDamageSource = true;
        m_lastDamageStamp = tickCount;

        if (attacker) SetLastHurtByMob(attacker);

        if (tookFullDamage) {
            hurtMarked = true;

            // Knockback away from the attacker. MC jitters a degenerate
            // direction rather than skipping, so a hit landed from exactly
            // overhead still pushes.
            //
            // Gated on MC's #no_knockback damage-type tag. Explosions are in
            // it, and that is load-bearing rather than cosmetic: an explosion
            // already applies its OWN push, scaled by how much of you it can
            // actually see. This generic shove has no line-of-sight test at
            // all, so leaving it on meant a blast on the far side of an
            // obsidian wall still threw you across the room.
            if (attacker && !DamageSourceHasNoKnockback(source)) {
                // MC dealDefaultKnockback: a PROJECTILE pushes along its own
                // flight (calculateHorizontalHurtKnockbackDirection = its
                // delta movement, negated into the away-from vector); any
                // other blow pushes away from the source position — the
                // direct entity's when there is one, else the attacker's.
                double dx, dz;
                if (const Entity* direct = m_hurtDirectEntity;
                    direct && direct != attacker && dynamic_cast<const Projectile*>(direct)) {
                    dx = -direct->velocity.x;
                    dz = -direct->velocity.z;
                } else {
                    const Entity* from = m_hurtDirectEntity ? m_hurtDirectEntity : attacker;
                    dx = from->position.x - position.x;
                    dz = from->position.z - position.z;
                }
                Knockback(kDefaultKnockback, dx, dz);
            }
        }

        if (IsDeadOrDying()) {
            Die(source, attacker);
        }

        return true;
    }

    void LivingEntity::Knockback(double power, double dx, double dz) {
        power *= 1.0 - GetAttributeValue(Attribute::KnockbackResistance);
        if (power <= 0.0) return;

        needsSync = true;

        // MC LOOPS the jitter until the direction is usable (`while (dx*dx +
        // dz*dz < 1.0E-5)`) — a single roll then a fixed fallback both
        // diverges the RNG stream and biases the push direction.
        if (JavaRandom* rng = m_level ? &m_level->Random() : nullptr) {
            while (dx * dx + dz * dz < 1.0e-5) {
                dx = (rng->NextDouble() - rng->NextDouble()) * 0.01;
                dz = (rng->NextDouble() - rng->NextDouble()) * 0.01;
            }
        } else if (dx * dx + dz * dz < 1.0e-5) {
            dx = 0.01;
        }

        const double len = std::sqrt(dx * dx + dz * dz);
        const double kx = dx / len * power;
        const double kz = dz / len * power;

        // MC halves the existing motion and subtracts the impulse, so a victim
        // already moving is not simply overwritten.
        velocity.x = velocity.x / 2.0 - kx;
        velocity.z = velocity.z / 2.0 - kz;
        if (onGround) {
            velocity.y = std::min(0.4, velocity.y / 2.0 + power);
        }
    }

    void LivingEntity::Die(MobDamageSource source, Entity* attacker) {
        if (m_dead) return;
        m_dead = true;
        deathTime = 0;

        if (m_level && !m_level->IsClientSide()) {
            // Entity event 3 — the client plays the death animation and sound.
            m_level->BroadcastEntityEvent(*this, 3);
        }
    }

    void LivingEntity::HandleEntityEvent(uint8_t id) {
        if (id == 60) {
            MakePoofParticles();
        } else if (id == kEntityEventSwing) {
            // The server's melee whack (MC's Animate packet stand-in) — run
            // the local swing clock. Client side, so Swing() won't rebroadcast.
            Swing();
        } else {
            Entity::HandleEntityEvent(id);
        }
    }

    void LivingEntity::MakePoofParticles() {
        // MC LivingEntity.makePoofParticles, verbatim: 20 POOF, gaussian *
        // 0.02 velocity, spawned at a random body point offset backwards by
        // velocity * 10. Java argument order preserved (xa, ya, za, then the
        // three position draws).
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        const double w = static_cast<double>(GetBbWidth());
        const double h = static_cast<double>(GetBbHeight());
        for (int i = 0; i < 20; ++i) {
            const double xa = rng.NextGaussian() * 0.02;
            const double ya = rng.NextGaussian() * 0.02;
            const double za = rng.NextGaussian() * 0.02;
            const double px = position.x + w * (2.0 * rng.NextDouble() - 1.0);
            const double py = position.y + h * rng.NextDouble();
            const double pz = position.z + w * (2.0 * rng.NextDouble() - 1.0);
            m_level->AddParticle(ParticleKind::Poof,
                                 px - xa * 10.0, py - ya * 10.0, pz - za * 10.0,
                                 xa, ya, za);
        }
    }

    void LivingEntity::TickDeath() {
        ++deathTime;
        // MC removes the entity after the 20-tick fall-over animation, sending
        // event 60 (poof particles) as it goes.
        if (deathTime >= kDeathDuration && m_level && !m_level->IsClientSide() && !IsRemoved()) {
            m_level->BroadcastEntityEvent(*this, 60);
            Remove(RemovalReason::Killed);
        }
    }

} // namespace Game
