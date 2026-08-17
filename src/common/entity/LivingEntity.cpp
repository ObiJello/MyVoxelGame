// File: src/common/entity/LivingEntity.cpp
#include "common/entity/LivingEntity.hpp"
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

    LivingEntity::~LivingEntity() = default;

    void LivingEntity::SetHealth(float h) {
        m_health = std::clamp(h, 0.0f, GetMaxHealth());
    }

    bool LivingEntity::IsEffectiveAi() const {
        return m_level && !m_level->IsClientSide();
    }

    void LivingEntity::SetLastHurtByMob(Entity* e) {
        m_lastHurtByMob = e;
        m_lastHurtByMobTimestamp = m_level ? m_level->GetGameTime() : 0;
    }

    void LivingEntity::ClearReferenceTo(const Entity* entity) {
        if (m_lastHurtByMob == entity) m_lastHurtByMob = nullptr;
        if (m_lastHurtMob == entity)   m_lastHurtMob = nullptr;
    }

    void LivingEntity::Swing() {
        // MC LivingEntity.swing — restart the arm swing unless one is already
        // more than half done, so rapid attacks do not look frozen.
        if (!swinging || swingTime >= 3 || swingTime < 0) {
            swingTime = -1;
            swinging = true;
        }
    }

    void LivingEntity::UpdateSwingTime() {
        // MC LivingEntity.updateSwingTime. The duration is derived from attack
        // speed in MC; with no haste effects modelled it is the flat 6 ticks
        // that an unmodified ATTACK_SPEED produces.
        constexpr int kSwingDuration = 6;
        if (swinging) {
            ++swingTime;
            if (swingTime >= kSwingDuration) {
                swingTime = 0;
                swinging = false;
            }
        } else {
            swingTime = 0;
        }
        attackAnim = static_cast<float>(swingTime) / static_cast<float>(kSwingDuration);
    }

    float LivingEntity::GetJumpPower() const {
        // MC LivingEntity.getJumpPower: JUMP_STRENGTH * blockJumpFactor.
        // No block has a jump factor in this engine (honey/slime jump damping
        // is not modelled), so the factor is 1.
        return static_cast<float>(GetAttributeValue(Attribute::JumpStrength));
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

        // MC Entity.getBlockPosBelowThatAffectsMyMovement -> getOnPos(0.2F).
        const int belowY = static_cast<int>(std::floor(position.y - 0.2));
        const int belowX = static_cast<int>(std::floor(position.x));
        const int belowZ = static_cast<int>(std::floor(position.z));

        float blockFriction = 1.0f;
        if (onGround && m_level && m_level->Blocks()) {
            blockFriction = GetBlockFriction(m_level->Blocks()->GetBlock(belowX, belowY, belowZ));
        }

        if (inFluid) {
            // MC travelInWater: horizontal drag 0.8 (0.9 sprinting), a fixed
            // 0.02 acceleration, and gravity reduced to a sixteenth.
            const float waterDrag = IsSprinting() ? 0.9f : 0.8f;
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity.x *= waterDrag;
            velocity.z *= waterDrag;
            velocity.y *= 0.8;
            if (!IsCreative()) velocity.y -= GetGravity() / 16.0;
            return;
        }

        const float friction = blockFriction * kFrictionBase;

        // MC handleRelativeFrictionAndCalculateMovement.
        const float accel = onGround
            ? m_speed * (kGroundAccel / (blockFriction * blockFriction * blockFriction))
            : kFlyingSpeed;
        MoveRelative(accel, input);
        Move(velocity);

        double movementY = velocity.y - GetGravity();

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
            velocity.x *= friction;
            velocity.y  = movementY * kVerticalDrag;
            velocity.z *= friction;
        }
    }

    void LivingEntity::UpdateWalkAnimation(float distance) {
        const float targetSpeed = std::min(distance * 4.0f, 1.0f);
        walkAnimation.Update(targetSpeed, 0.4f, IsBaby() ? 3.0f : 1.0f);
    }

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

        // MC applies the steering decay HERE — after serverAiStep and after
        // the jump block, immediately before travel. The position matters:
        // Mob::SetSpeed writes zza every tick the MoveControl is steering, so
        // decaying beforehand (as this used to) scaled a value that was about
        // to be overwritten and the 0.98 never reached travel at all. Mobs
        // walked ~2% faster than vanilla as a result.
        xxa *= kInputFriction;
        zza *= kInputFriction;

        // ── Travel ─────────────────────────────────────────────────────────
        const glm::dvec3 input(xxa, yya, zza);
        if (IsEffectiveAi() || (m_level && m_level->IsClientSide())) {
            Travel(input);
        }

        if (m_level && m_level->IsClientSide()) {
            CalculateEntityAnimation(false);
        }
    }

    void LivingEntity::TickCombatTimers() {
        if (hurtTime > 0) --hurtTime;
        if (m_invulnerableTime > 0) --m_invulnerableTime;

        // MC clears the "who hurt me" memory after 100 ticks, which is what
        // makes a mob eventually forget an attacker it never reached.
        if (m_lastHurtByMob && m_level &&
            m_level->GetGameTime() - m_lastHurtByMobTimestamp > 100) {
            m_lastHurtByMob = nullptr;
        }
    }

    void LivingEntity::BaseTick() {
        oAttackAnim = attackAnim;

        Entity::BaseTick();

        TickCombatTimers();

        // Fire damage: 1.0 every 20 ticks while burning.
        if (m_remainingFireTicks > 0 && IsEffectiveAi() && tickCount % 20 == 0) {
            Hurt(MobDamageSource::Fire, 1.0f, nullptr);
        }

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

    void LivingEntity::ActuallyHurt(MobDamageSource source, float amount, Entity* attacker) {
        amount = GetDamageAfterArmorAbsorb(source, amount);
        if (amount <= 0.0f) return;
        SetHealth(m_health - amount);
    }

    bool LivingEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (IsRemoved() || IsDeadOrDying()) return false;
        if (amount < 0.0f) amount = 0.0f;

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

        if (attacker) SetLastHurtByMob(attacker);

        if (tookFullDamage) {
            hurtMarked = true;

            // Knockback away from the attacker. MC jitters a degenerate
            // direction rather than skipping, so a hit landed from exactly
            // overhead still pushes.
            if (attacker) {
                const double dx = attacker->position.x - position.x;
                const double dz = attacker->position.z - position.z;
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

        // MC jitters a degenerate direction until it has a usable one.
        if (dx * dx + dz * dz < 1.0e-5) {
            JavaRandom* rng = m_level ? &m_level->Random() : nullptr;
            const double jx = rng ? (rng->NextDouble() - rng->NextDouble()) * 0.01 : 0.01;
            const double jz = rng ? (rng->NextDouble() - rng->NextDouble()) * 0.01 : 0.0;
            dx = jx;
            dz = jz;
            if (dx * dx + dz * dz < 1.0e-9) dx = 0.01;
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
