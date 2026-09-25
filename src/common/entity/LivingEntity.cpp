// File: src/common/entity/LivingEntity.cpp
#include "common/entity/LivingEntity.hpp"
#include "common/world/block/BlockBounce.hpp"
#include "common/world/block/BlockFriction.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/core/Mth.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/sound/EntitySounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/sound/SoundType.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

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
        for (MobEffectInstance& e : EffectStorage()) {
            if (e.effect == effect) return &e;
        }
        return nullptr;
    }

    const MobEffectInstance* LivingEntity::GetEffect(MobEffectId effect) const {
        return FindEffectIn(EffectStorage(), effect);
    }

    bool LivingEntity::CanBeAffected(const MobEffectInstance& effect) const {
        // MC LivingEntity.canBeAffected, in its order. The tags are one type
        // each in vanilla data: #immune_to_infested = silverfish,
        // #immune_to_oozing = slime; IGNORES_POISON_AND_REGEN is the #undead
        // set — the same membership as inverted heal-and-harm.
        if (GetType() == EntityTypeId::Silverfish) return effect.effect != MobEffectId::Infested;
        if (GetType() == EntityTypeId::Slime)      return effect.effect != MobEffectId::Oozing;
        if (!IsInvertedHealAndHarm()) return true;
        return effect.effect != MobEffectId::Regeneration &&
               effect.effect != MobEffectId::Poison;
    }

    double LivingEntity::GetVisibilityPercent(const Entity* targetingEntity) const {
        // MC LivingEntity.getVisibilityPercent, in its order. isInvisible()
        // is the shared flag MC's updateInvisibilityStatus derives from the
        // INVISIBILITY effect; on the server that is the effect itself.
        double visibilityPercent = 1.0;
        if (IsDiscrete()) visibilityPercent *= 0.8;
        if (HasEffect(MobEffectId::Invisibility)) {
            float coverPercentage = GetArmorCoverPercentage();
            if (coverPercentage < 0.1f) coverPercentage = 0.1f;
            visibilityPercent *= 0.7 * static_cast<double>(coverPercentage);
        }
        if (targetingEntity) {
            visibilityPercent *= GetEquipmentVisibilityFactor(targetingEntity);
        }
        return std::clamp(visibilityPercent, 0.0, 10.0);
    }

    bool LivingEntity::AddEffect(MobEffectInstance effect, Entity* source) {
        // MC LivingEntity.addEffect(newEffect, source).
        if (!CanBeAffected(effect)) return false;

        const MobEffectId id = effect.effect;
        const int amplifier = effect.amplifier;
        MobEffectInstance* existing = FindEffect(id);
        bool changed = false;
        if (!existing) {
            std::vector<MobEffectInstance>& effects = EffectStorage();
            effects.push_back(std::move(effect));
            OnEffectAdded(effects.back(), source);
            changed = true;
            // MC newEffect.onEffectAdded(this) → MobEffect.onEffectAdded: the
            // soundOnAdded (BAD_OMEN / TRIAL_OMEN / RAID_OMEN chimes) at the
            // mob's feet, volume 1, pitch 1, through the level's playSound.
            if (m_level && !m_level->IsClientSide()) {
                if (const char* sound = GetEffectSoundOnAdded(id)) {
                    m_level->PlaySound(nullptr, position, sound, GetSoundSource(), 1.0f, 1.0f);
                }
            }
        } else if (existing->Update(effect)) {
            OnEffectUpdated(*existing, /*refreshAttributes=*/true, source);
            changed = true;
        }
        // MC: newEffect.onEffectStarted(this) — on EVERY call, landed or not,
        // with the NEW instance's amplifier (ABSORPTION's top-up).
        if (m_level && !m_level->IsClientSide()) OnEffectStarted(*this, id, amplifier);
        return changed;
    }

    void LivingEntity::ForceAddEffect(MobEffectInstance effect, Entity* source) {
        // MC LivingEntity.forceAddEffect: activeEffects.put, the previous
        // instance's blend state carried over.
        if (!CanBeAffected(effect)) return;
        if (MobEffectInstance* existing = FindEffect(effect.effect)) {
            effect.CopyBlendState(*existing);
            *existing = MobEffectInstance(effect);
            existing->CopyBlendState(effect);
            OnEffectUpdated(*existing, /*refreshAttributes=*/true, source);
        } else {
            std::vector<MobEffectInstance>& effects = EffectStorage();
            effects.push_back(std::move(effect));
            OnEffectAdded(effects.back(), source);
        }
    }

    bool LivingEntity::RemoveEffect(MobEffectId effect) {
        // MC removeEffect → removeEffectNoUpdate + onEffectsRemoved.
        std::vector<MobEffectInstance>& effects = EffectStorage();
        for (size_t i = 0; i < effects.size(); ++i) {
            if (effects[i].effect != effect) continue;
            MobEffectInstance removed = std::move(effects[i]);
            effects.erase(effects.begin() + static_cast<ptrdiff_t>(i));
            OnEffectRemoved(removed);
            RefreshEffectAttributes();
            return true;
        }
        return false;
    }

    bool LivingEntity::RemoveAllEffects() {
        // MC removeAllEffects: false on the client and on an empty map.
        if (m_level && m_level->IsClientSide()) return false;
        std::vector<MobEffectInstance>& effects = EffectStorage();
        if (effects.empty()) return false;
        std::vector<MobEffectInstance> removed = std::move(effects);
        effects.clear();
        for (const MobEffectInstance& e : removed) OnEffectRemoved(e);
        RefreshEffectAttributes();
        return true;
    }

    void LivingEntity::RestoreEffects(std::vector<MobEffectInstance> effects) {
        // Drop the modifiers the old set installed before replacing it, so a
        // reload onto an entity that already had effects cannot stack them.
        std::vector<MobEffectInstance>& storage = EffectStorage();
        for (const auto& e : storage) {
            RemoveEffectAttributeModifiers(m_attributes, e.effect);
        }
        storage = std::move(effects);
        for (const auto& e : storage) {
            AddEffectAttributeModifiers(m_attributes, e.effect, e.amplifier);
        }
        m_effectsDirty = true;   // MC readAdditionalSaveData sets effectsDirty
    }

    void LivingEntity::TriggerOnDeathMobEffects(RemovalReason reason) {
        // MC triggerOnDeathMobEffects: each effect's onMobRemoved, then clear
        // — WITHOUT onEffectsRemoved (the entity is going away; MC sends no
        // remove packets either, the client drops the whole entity).
        if (!m_level || m_level->IsClientSide()) return;
        std::vector<MobEffectInstance> effects = EffectStorage();   // copies: a hook may spawn
        for (const MobEffectInstance& e : effects) {
            OnEffectMobRemoved(*this, e.effect, e.amplifier, reason);
        }
        EffectStorage().clear();
        m_effectsDirty = true;
    }

    void LivingEntity::OnEffectAdded(const MobEffectInstance& effect, Entity* source) {
        // MC onEffectAdded: server side only.
        (void)source;
        if (m_level && m_level->IsClientSide()) return;
        m_effectsDirty = true;
        AddEffectAttributeModifiers(m_attributes, effect.effect, effect.amplifier);
    }

    void LivingEntity::OnEffectUpdated(const MobEffectInstance& effect,
                                       bool refreshAttributes, Entity* source) {
        (void)source;
        if (m_level && m_level->IsClientSide()) return;
        m_effectsDirty = true;
        if (refreshAttributes) {
            RemoveEffectAttributeModifiers(m_attributes, effect.effect);
            AddEffectAttributeModifiers(m_attributes, effect.effect, effect.amplifier);
            RefreshEffectAttributes();
        }
    }

    void LivingEntity::OnEffectRemoved(const MobEffectInstance& effect) {
        if (m_level && m_level->IsClientSide()) return;
        m_effectsDirty = true;
        RemoveEffectAttributeModifiers(m_attributes, effect.effect);
    }

    void LivingEntity::RefreshEffectAttributes() {
        // MC refreshDirtyAttributes → onAttributeUpdated: a lowered
        // MAX_HEALTH clamps health (HEALTH_BOOST running out), a lowered
        // MAX_ABSORPTION clamps the absorption hearts (ABSORPTION ending).
        const float maxHealth = GetMaxHealth();
        if (GetHealth() > maxHealth) SetHealth(maxHealth);
        const float maxAbsorption = GetMaxAbsorption();
        if (GetAbsorptionAmount() > maxAbsorption) SetAbsorptionAmount(maxAbsorption);
    }

    const EffectVisuals& LivingEntity::GetEffectVisuals() const {
        // MC updateDirtyEffects, pulled lazily by whoever syncs. The client's
        // copy is never dirty — it is whatever the server last sent.
        if (m_effectsDirty && !(m_level && m_level->IsClientSide())) {
            m_effectVisuals = EffectVisuals::FromEffects(
                EffectStorage(), HasEffect(MobEffectId::Glowing));
            m_effectsDirty = false;
        }
        return m_effectVisuals;
    }

    bool LivingEntity::IsCurrentlyGlowing() const {
        if (m_level && !m_level->IsClientSide()) return HasEffect(MobEffectId::Glowing);
        return m_effectVisuals.Glowing();
    }

    void LivingEntity::TickEffects() {
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // MC tickEffects, client branch: count the (predicted) effects
            // down and roll the ambient swirl from the synched particles.
            // Client mobs hold no effect list; the local player's list lives
            // on ClientPlayer.
            for (MobEffectInstance& e : EffectStorage()) e.TickClient();
            if (!IsRemoved()) {
                SpawnEffectParticles(*m_level, m_effectVisuals, position,
                                     GetBbWidth(), GetBbHeight());
            }
            return;
        }

        // MC tickEffects, server branch: MobEffectInstance.tickServer per
        // effect over the LIVE map. The apply step is run here rather than
        // inside TickServer because it can change the list under the loop —
        // a killing WITHER / INSTANT_DAMAGE tick spends a totem, whose
        // clear-all empties it — and an instance ticked in place would then
        // be written after it was freed. MC walks the map with an iterator
        // and swallows the ConcurrentModificationException: a structural
        // change ends the pass, the in-flight instance having already ticked
        // down if it is still in the map. That is reproduced exactly.
        std::vector<MobEffectInstance>& effects = EffectStorage();
        for (size_t i = 0; i < effects.size();) {
            const MobEffectId id = effects[i].effect;
            bool keep = effects[i].HasRemainingDuration();
            if (keep) {
                const int amplifier = effects[i].amplifier;
                const int cadence = effects[i].CadenceTickCount(tickCount);
                if (ShouldApplyEffectTickThisTick(id, cadence, amplifier)) {
                    const size_t sizeBefore = effects.size();
                    keep = ApplyEffectTick(*this, id, amplifier);
                    if (effects.size() != sizeBefore || i >= effects.size() ||
                        effects[i].effect != id) {
                        if (keep) {
                            if (MobEffectInstance* still = FindEffect(id)) {
                                bool surfaced = false;
                                still->FinishServerTick(surfaced);
                                if (surfaced) OnEffectUpdated(*still, true, nullptr);
                            }
                        }
                        break;
                    }
                }
            }
            bool downgraded = false;
            if (keep) keep = effects[i].FinishServerTick(downgraded);
            if (!keep) {
                MobEffectInstance removed = std::move(effects[i]);
                effects.erase(effects.begin() + static_cast<ptrdiff_t>(i));
                OnEffectRemoved(removed);
                RefreshEffectAttributes();
            } else {
                // MC's onEffectUpdate runnable: a hidden effect surfacing must
                // re-fold the attribute modifiers at the new amplifier.
                if (downgraded) {
                    OnEffectUpdated(effects[i], true, nullptr);
                } else if (effects[i].duration % 600 == 0) {
                    // MC: every 30 s an unchanged effect is re-sent (the
                    // client's countdown drifts from the server's).
                    OnEffectUpdated(effects[i], false, nullptr);
                }
                ++i;
            }
        }
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
        if (HasDigSpeed(EffectStorage())) {
            // MobEffectUtil.hasDigSpeed: HASTE or CONDUIT_POWER, the larger
            // of the two amplifiers.
            swingDuration -= 1 + GetDigSpeedAmplification(EffectStorage());
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
        PROFILE_ZONE_N("Living.Travel");
        // MC LivingEntity.travel dispatches to travelInFluid / travelFallFlying
        // / travelInAir; the fluid branch is below, the air path follows.
        //
        // shouldTravelInFluid(getFluidState(blockPosition())): in a liquid,
        // affected by fluids, and not standing on this one (the strider).
        bool travelInFluid = false;
        if (IsInLiquid() && IsAffectedByFluids() && m_level && m_level->Blocks()) {
            const glm::ivec3 bp = BlockPosition();
            travelInFluid = !CanStandOnFluid(GetFluidState(*m_level->Blocks(), bp));
        }

        // MC Entity.getBlockPosBelowThatAffectsMyMovement -> getOnPos(0.500001F)
        // — half a block down (plus an epsilon so an exact block boundary
        // rounds DOWN), not 0.2 (that is getOnPosLegacy). The difference is
        // which block's slipperiness a mob on a slab above ice reads.
        const int belowY = static_cast<int>(std::floor(position.y - 0.500001));
        const int belowX = static_cast<int>(std::floor(position.x));
        const int belowZ = static_cast<int>(std::floor(position.z));

        float blockFriction = 1.0f;
        if (onGround && m_level && m_level->Blocks()) {
            // Motion-aware: quicksoil is FrictionCapped (BlockFriction.hpp).
            blockFriction = GetBlockFriction(m_level->Blocks()->GetBlock(belowX, belowY, belowZ),
                                             velocity);
            // MC 26.3: computeModifiedFriction(friction, FRICTION_MODIFIER) —
            // the modifier scales how far the block is from frictionless,
            // 1.0 (every mob but the sulfur cube's archetypes) is identity.
            blockFriction = ComputeModifiedFriction(
                blockFriction, static_cast<float>(GetAttributeValue(Attribute::FrictionModifier)));
        }

        if (travelInFluid) {
            // MC travelInFluid: isFalling, oldY and the base gravity are
            // captured BEFORE the move.
            const bool   isFalling   = velocity.y <= 0.0;
            const double oldY        = position.y;
            const double baseGravity = (IsCreative() || IsNoGravity()) ? 0.0 : GetEffectiveGravity();

            // MC getFluidFallingAdjustedMovement: gravity at a sixteenth,
            // with the -0.003 snap that stops a sinking mob jittering at
            // the point where the drag and the gravity term cancel. Off
            // entirely while sprinting (a sprint-swimming mob does not sink).
            const auto fluidFallingAdjusted = [&](glm::dvec3 movement) {
                if (baseGravity != 0.0 && !IsSprinting()) {
                    double yd;
                    if (isFalling && std::abs(movement.y - 0.005) >= 0.003 &&
                        std::abs(movement.y - baseGravity / 16.0) < 0.003) {
                        yd = -0.003;
                    } else {
                        yd = movement.y - baseGravity / 16.0;
                    }
                    movement.y = yd;
                }
                return movement;
            };

            if (IsInWater()) {
                // MC travelInWater. Not carried: WATER_MOVEMENT_EFFICIENCY
                // (depth strider) — no such attribute here. DOLPHINS_GRACE
                // replaces the slow-down outright.
                float slowDown = IsSprinting() ? 0.9f : 0.8f;   // getWaterSlowDown
                if (HasEffect(MobEffectId::DolphinsGrace)) slowDown = 0.96f;
                const float speed    = 0.02f;
                MoveRelative(speed, input);
                Move(velocity);
                glm::dvec3 movement = velocity;
                // `horizontalCollision && onClimbable()` → y = 0.2: a ladder
                // in water is still a ladder. No climbable model here.
                movement.x *= slowDown;
                movement.y *= 0.800000011920929;
                movement.z *= slowDown;
                velocity = fluidFallingAdjusted(movement);
            } else {
                // MC travelInLava: 0.02 acceleration, then either the shallow
                // branch (drag 0.5/0.8/0.5 and the sixteenth-gravity
                // adjustment) or the deep one (everything halved), then a
                // QUARTER gravity on top — lava is four times as sticky
                // downward as water.
                MoveRelative(0.02f, input);
                Move(velocity);
                if (IsInShallowFluid(FluidType::Lava)) {
                    velocity.x *= 0.5;
                    velocity.y *= 0.800000011920929;
                    velocity.z *= 0.5;
                    velocity = fluidFallingAdjusted(velocity);
                } else {
                    velocity *= 0.5;
                }
                if (baseGravity != 0.0) velocity.y -= baseGravity / 4.0;
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
        // MC 26.3 Entity.restituteMovementAfterCollisions, both halves: the
        // ENTITY's bounciness (a sulfur cube wearing a block) on every
        // collided axis, and on a landing the BLOCK's bounceRestitution too
        // (a bed, a slime block, a shelf mushroom — BlockBounce.hpp), the
        // larger of the two winning. A collided axis keeps -v * restitution
        // instead of the zero Move() left there. `preMove` is MC's
        // currentMovement (deltaMovement before the move), `moved` the
        // movement that actually happened.
        //
        // isSuppressingBounce is isShiftKeyDown: a crouching entity neither
        // bounces off a block nor by itself. This is a LivingEntity, so the
        // block's number is taken whole (Entity.getBlockBounciness's ×0.8 is
        // for items and TNT, which do not come through here).
        const bool suppressing = GetPose() == Pose::Crouching;
        double restitution = suppressing ? 0.0 : GetEntityBounciness();
        const bool xCollision = preMove.x != 0.0 && velocity.x == 0.0;
        const bool zCollision = preMove.z != 0.0 && velocity.z == 0.0;
        if (!verticalCollision && !xCollision && !zCollision) return;

        glm::dvec3 after = velocity;
        if (xCollision) after.x = -preMove.x * restitution;
        if (zCollision) after.z = -preMove.z * restitution;
        bool bounced = restitution > 0.0 && (xCollision || zCollision);

        if (verticalCollision) {
            double r = restitution;
            if (preMove.y < 0.0) {
                // verticalCollisionBelow: the block landed on (getOnPos(0.2))
                // joins in, unless it is honey (SUPPRESSES_BOUNCE), and the
                // whole bounce is off below one tick of gravity — a resting
                // mob does not vibrate on the floor.
                double blockRestitution = 0.0;
                bool blockSuppresses = false;
                if (const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr) {
                    const glm::ivec3 p = BlockPosition();
                    const BlockID landedOn = blocks->GetBlock(
                        p.x, static_cast<int>(std::floor(position.y - 0.2)), p.z);
                    blockSuppresses  = SuppressesBounce(landedOn);
                    blockRestitution = BounceRestitution(landedOn);
                }
                r = (-preMove.y > GetEffectiveGravity() && !suppressing && !blockSuppresses)
                        ? std::max(r, blockRestitution) : 0.0;
            }
            double gravityCompensation = 0.0;
            double effectiveDrag = 1.0;
            if (r > 0.0 && preMove.y != 0.0) {
                const double portionWithMovement = moved.y / preMove.y;
                gravityCompensation = portionWithMovement * GetEffectiveGravity();
                effectiveDrag = 1.0 + (static_cast<double>(airDrag) - 1.0) * portionWithMovement;
                bounced = true;
            }
            after.y = (gravityCompensation - preMove.y) * effectiveDrag * r;
        }
        if (!bounced) return;   // nothing to restitute: Move()'s zeros stand
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
        // MC LivingEntity.causeFallDamage: the fall-damage thud (big above 4)
        // and the landed-on block's fall sound, then the hurt. A player's
        // are its client's own (the player half of the sound port).
        if (!IsPlayer()) {
            const FallSounds sounds = GetFallSounds();
            PlaySound(damage > 4 ? sounds.big : sounds.small, 1.0f, 1.0f);
            PlayBlockFallSound();
        }
        Hurt(MobDamageSource::Fall, static_cast<float>(damage), nullptr);
        return true;
    }

    // ── Sound ───────────────────────────────────────────────────────────────

    const char* LivingEntity::GetHurtSound(MobDamageSource source) const {
        (void)source;
        return PickSound(EntitySoundsOf(GetType()).hurt, *this);
    }

    const char* LivingEntity::GetDeathSound() const {
        return PickSound(EntitySoundsOf(GetType()).death, *this);
    }

    float LivingEntity::GetSoundVolume() const {
        return EntitySoundsOf(GetType()).soundVolume;
    }

    float LivingEntity::GetVoicePitch() const {
        if (!m_level) return 1.0f;
        JavaRandom& rng = m_level->Random();
        const float spread = (rng.NextFloat() - rng.NextFloat()) * 0.2f;
        return IsBaby() ? spread + 1.5f : spread + 1.0f;
    }

    void LivingEntity::MakeSound(const char* event) {
        if (event && event[0]) PlaySound(event, GetSoundVolume(), GetVoicePitch());
    }

    LivingEntity::FallSounds LivingEntity::GetFallSounds() const {
        return {SoundEvents::GENERIC_SMALL_FALL, SoundEvents::GENERIC_BIG_FALL};
    }

    void LivingEntity::PlayBlockFallSound() {
        if (IsSilent() || !m_level) return;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return;
        const int x = static_cast<int>(std::floor(position.x));
        const int y = static_cast<int>(std::floor(position.y - 0.20000000298023224));
        const int z = static_cast<int>(std::floor(position.z));
        const BlockState state = blocks->GetBlockState(x, y, z);
        if (state.Block() == BlockID::Air) return;
        const SoundType& type = SoundTypeOf(state);
        PlaySound(type.GetFallSound(), type.GetVolume() * 0.5f, type.GetPitch() * 0.75f);
    }

    void LivingEntity::PlayItemBreakSound(const char* breakSound) {
        // MC: level.playLocalSound — the client of whoever sees it; the
        // server's copy is silent (EntityLevel's server bridge ignores local
        // sounds), exactly as vanilla's ServerLevel.playLocalSound.
        if (!breakSound || !breakSound[0] || IsSilent() || !m_level) return;
        JavaRandom& rng = m_level->Random();
        m_level->PlayLocalSound(position, breakSound, GetSoundSource(), 0.8f, 0.8f + rng.NextFloat() * 0.4f, false);
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
        PROFILE_ZONE_N("Living.AiStep");
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

        // ── Jump (MC aiStep's "jump" block) ────────────────────────────────
        //
        // The fluid height decides between swimming up (jumpInLiquid, +0.04
        // a tick) and a real jump off the floor: wading ankle-deep on the
        // ground is still a jump, and so is standing in shallow lava. Deep
        // enough (past getFluidJumpThreshold) or off the floor, it is a swim.
        if (jumping && IsAffectedByFluids()) {
            const double fluidHeight = IsInLava() ? GetFluidHeight(FluidType::Lava)
                                                  : GetFluidHeight(FluidType::Water);
            const bool   inWaterAndHasFluidHeight = IsInWater() && fluidHeight > 0.0;
            const double fluidJumpThreshold = GetFluidJumpThreshold();
            if (inWaterAndHasFluidHeight && (!onGround || fluidHeight > fluidJumpThreshold)) {
                velocity.y += kFluidJumpImpulse;                       // jumpInLiquid(WATER)
            } else if (!IsInLava() || (onGround && IsInShallowFluid(FluidType::Lava))) {
                if ((onGround || (inWaterAndHasFluidHeight && fluidHeight <= fluidJumpThreshold)) &&
                    m_noJumpDelay == 0) {
                    JumpFromGround();
                    m_noJumpDelay = kJumpDelay;
                }
            } else {
                velocity.y += kFluidJumpImpulse;                       // jumpInLiquid(LAVA)
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
        PROFILE_ZONE_N("Living.PushEntities");
        if (!m_level) return;
        std::vector<Entity*> list;
        m_level->GetEntitiesInBox(GetAABB(), this, list);
        // MC Level.getPushableEntities = EntitySelector.pushableBy(this):
        // pushable and alive (team no-push rules are scoreboard machinery,
        // absent).
        std::erase_if(list, [](Entity* e) { return !e->IsPushable(); });
        if (list.empty()) return;

        // MC max_entity_cramming (default 24): with more than max-1 pushable
        // non-passenger neighbours, 6.0 cramming damage on a 1-in-4 roll per
        // tick — the overcrowding valve cramming farms are built on. Zero
        // turns it off (LivingEntity.pushEntities: `if (maxCramming > 0 ...`).
        const int maxEntityCramming = Rules::GetInt(Rules::Id::MaxEntityCramming);
        if (maxEntityCramming > 0 &&
            static_cast<int>(list.size()) > maxEntityCramming - 1 &&
            m_level->Random().NextInt(4) == 0) {
            int count = 0;
            for (Entity* e : list) {
                if (!e->IsPassenger()) ++count;
            }
            if (count > maxEntityCramming - 1) {
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
        // MC decreaseAirSupply: `oxygenBonus > 0 && random.nextDouble() >=
        // 1 / (oxygenBonus + 1)` keeps the tick's air. OXYGEN_BONUS is the
        // RESPIRATION enchantment's attribute (a player's helmet, through
        // the view's equipment modifiers); with none the roll never happens.
        const double oxygenBonus = GetAttributeValue(Attribute::OxygenBonus);
        if (oxygenBonus > 0.0 && m_level &&
            m_level->Random().NextDouble() >= 1.0 / (oxygenBonus + 1.0)) {
            return currentSupply;
        }
        return currentSupply - 1;
    }

    void LivingEntity::IgniteForTicks(int ticks) {
        Entity::IgniteForTicks(static_cast<int>(std::ceil(
            static_cast<double>(ticks) * GetAttributeValue(Attribute::BurningTime))));
    }

    void LivingEntity::HandleUnderwaterAir() {
        // MC LivingEntity.baseTick, the isEyeInFluid(WATER) block. Bubble
        // columns (which exempt the eye block) do not exist in this engine.
        if (IsEyeInWater()) {
            // MC MobEffectUtil.hasWaterBreathing: WATER_BREATHING,
            // CONDUIT_POWER or BREATH_OF_THE_NAUTILUS.
            const bool canDrownInWater =
                !CanBreatheUnderwater() && !HasWaterBreathing(EffectStorage());
            if (canDrownInWater) {
                SetAirSupply(DecreaseAirSupply(GetAirSupply()));
                if (ShouldTakeDrowningDamage()) {
                    SetAirSupply(0);
                    // MC broadcasts entity event 67 here — the client's drown
                    // bubble particles; no particle system to land them in.
                    Hurt(MobDamageSource::Drown, 2.0f, nullptr);
                }
            } else if (GetAirSupply() < GetMaxAirSupply() &&
                       ShouldEffectsRefillAirSupply(EffectStorage())) {
                // MC shouldEffectsRefillAirsupply: BREATH_OF_THE_NAUTILUS
                // holds the breath without refilling it, unless water
                // breathing or conduit power is also present.
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
        // (blaze, zombified piglin) shed the ticks without the damage. MC
        // Entity.baseTick skips the on-fire tick while in lava — the lava
        // itself is already dealing its 4 through lavaHurt.
        if (m_remainingFireTicks > 0 && IsEffectiveAi() && tickCount % 20 == 0) {
            if (FireImmune()) {
                m_remainingFireTicks = 0;
            } else if (!IsInLava()) {
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

    float LivingEntity::GetDamageAfterArmorAbsorb(MobDamageSource source, float amount, Entity* attacker) {
        // MC CombatRules.getDamageAfterAbsorb. Armor toughness is in the
        // formula but is 0 for every mob here; keeping it spelled out means a
        // mob that gains armour later needs no change.
        const float armor = static_cast<float>(GetAttributeValue(Attribute::Armor));
        if (armor <= 0.0f) return amount;

        const float toughness = static_cast<float>(GetAttributeValue(Attribute::ArmorToughness));
        const float f = 2.0f + toughness / 4.0f;
        const float g = std::clamp(armor - amount / f, armor * 0.2f, 20.0f);
        float armorFraction = g / 25.0f;
        // source.getWeaponItem() (the direct entity's weapon): its
        // armor_effectiveness effects (Breach) rewrite the fraction, clamped
        // to [0, 1]. Server only, as MC's `level instanceof ServerLevel`.
        Entity* const direct = m_hurtDirectEntity ? m_hurtDirectEntity : attacker;
        if (direct && m_level && !m_level->IsClientSide()) {
            if (ItemStack* weapon = direct->GetWeaponItem(); weapon && !weapon->IsEmpty()) {
                const DamageSourceInfo info = DamageSourceInfo::Of(source, attacker, m_hurtDirectEntity);
                armorFraction = std::clamp(EnchantmentHelper::ModifyArmorEffectiveness(
                                               *m_level, *weapon, *this, info, armorFraction),
                                           0.0f, 1.0f);
            }
        }
        return amount * (1.0f - armorFraction);
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
        if (!bypassesArmor) amount = GetDamageAfterArmorAbsorb(source, amount, attacker);
        amount = GetDamageAfterMagicAbsorb(source, amount, attacker);
        // getDamageAfterMagicAbsorb's tail: unless #bypasses_effects or
        // #bypasses_enchantments, the worn enchantments' damage_protection
        // (EnchantmentHelper.getDamageProtection), capped at 20 — at most
        // 80 % off (CombatRules.getDamageAfterMagicAbsorb).
        if (amount > 0.0f && HasEquipmentSlots() && m_level && !m_level->IsClientSide()) {
            const DamageSourceInfo info = DamageSourceInfo::Of(source, attacker, m_hurtDirectEntity);
            if (!info.Is("minecraft:bypasses_effects") && !info.Is("minecraft:bypasses_enchantments")) {
                const float protection = EnchantmentHelper::GetDamageProtection(
                    *m_level, *this, info, EnchantmentEquipment::Of(*this));
                if (protection > 0.0f) amount *= 1.0f - std::clamp(protection, 0.0f, 20.0f) / 25.0f;
            }
        }
        // MC actuallyHurt: the absorption hearts (ABSORPTION's) soak the hit
        // first; whatever gets through comes off health.
        const float originalDamage = amount;
        amount = std::max(amount - GetAbsorptionAmount(), 0.0f);
        SetAbsorptionAmountClamped(GetAbsorptionAmount() - (originalDamage - amount));
        if (amount <= 0.0f) return;
        SetHealth(m_health - amount);
        SetAbsorptionAmountClamped(GetAbsorptionAmount() - amount);
    }

    bool LivingEntity::HurtFrom(MobDamageSource source, float amount, Entity* causingEntity,
                                Entity* directEntity) {
        Entity* const previous = m_hurtDirectEntity;
        m_hurtDirectEntity = directEntity;
        const bool hit = Hurt(source, amount, causingEntity);
        m_hurtDirectEntity = previous;
        return hit;
    }

    void LivingEntity::LavaHurt() {
        // MC Entity.lavaHurt: `if (!fireImmune()) hurtServer(lava, 4.0F)`
        // — the hurt cooldown inside Hurt is what spaces the hits.
        if (FireImmune()) return;
        if (!m_level || m_level->IsClientSide()) return;
        // ...and a landed hit sizzles (GENERIC_BURN, 0.4, 2.0..2.4).
        if (Hurt(MobDamageSource::Lava, 4.0f, nullptr) && !IsSilent()) {
            m_level->PlaySound(nullptr, position, SoundEvents::GENERIC_BURN, GetSoundSource(),
                               0.4f, 2.0f + m_level->Random().NextFloat() * 0.4f);
        }
    }

    bool LivingEntity::IsFireDamage(MobDamageSource source) const {
        // DamageTypeTags.IS_FIRE over this engine's sources. The fireball
        // damage types (DamageSources.fireball) arrive here as a Projectile
        // hit whose DIRECT entity is the fireball (Projectile::DealHitDamage
        // → HurtFrom), so they are recognised by that entity's type. (hot
        // floor and campfire have no damage source in this engine.)
        if (source == MobDamageSource::Fire || source == MobDamageSource::Lava) return true;
        if (source == MobDamageSource::Projectile && m_hurtDirectEntity) {
            const EntityTypeId t = m_hurtDirectEntity->GetType();
            return t == EntityTypeId::SmallFireball || t == EntityTypeId::Fireball;
        }
        return false;
    }

    bool LivingEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (IsRemoved() || IsDeadOrDying()) return false;
        if (amount < 0.0f) amount = 0.0f;

        // MC LivingEntity.hurtServer: isInvulnerableTo refuses an IS_FIRE
        // source on a fireImmune() type, then FIRE_RESISTANCE blanks every
        // IS_FIRE source outright — no i-frames consumed, no knockback, no
        // hurt flash. #is_fire holds on_fire, in_fire, lava AND the two
        // fireball impacts (a blaze's small fireball, a ghast's large one) —
        // which is why fire resistance shrugs off a blaze volley.
        if (IsFireDamage(source)) {
            if (FireImmune()) return false;
            if (HasEffect(MobEffectId::FireResistance)) return false;
        }

        // MC LivingEntity.isInvulnerableTo → EnchantmentHelper
        // .isImmuneToDamage: a worn enchantment's damage_immunity (Frost
        // Walker against #burn_from_stepping) refuses the hit outright.
        if (HasEquipmentSlots() && m_level && !m_level->IsClientSide()) {
            const DamageSourceInfo info = DamageSourceInfo::Of(source, attacker, m_hurtDirectEntity);
            if (EnchantmentHelper::IsImmuneToDamage(*m_level, *this, info, EnchantmentEquipment::Of(*this))) {
                return false;
            }
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

        // MC hurtServer: the death cry (on a full hit) then die(), or the
        // hurt sound. Server-side makeSound → Entity.playSound → everyone
        // near. A player's own voice belongs to the player half of the port
        // (MC Player.getHurtSound / LocalPlayer), so a player view is quiet.
        const bool voiced = m_level && !m_level->IsClientSide() && !IsPlayer();
        if (IsDeadOrDying()) {
            if (voiced && tookFullDamage) MakeSound(GetDeathSound());
            m_killerId = attacker ? attacker->GetId() : -1;
            m_killerDirectId = m_hurtDirectEntity ? m_hurtDirectEntity->GetId() : m_killerId;
            Die(source, attacker);
        } else if (voiced && tookFullDamage) {
            PlayHurtSound(source);
        }

        // MC hurtServer: on a successful hit every active effect hears about
        // it (MobEffect.onMobHurt — INFESTED's silverfish). Over a copy: a
        // spawned silverfish must not invalidate the iteration.
        if (m_level && !m_level->IsClientSide() && !EffectStorage().empty()) {
            const std::vector<MobEffectInstance> effects = EffectStorage();
            for (const MobEffectInstance& e : effects) {
                OnEffectMobHurt(*this, e.effect, e.amplifier);
            }
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
        // 60: LivingEntity's death poof. 20: MC Mob.handleEntityEvent's
        // spawnAnim — a monster spawner's new mob arrives in the same puff
        // (only mobs are ever sent it).
        if (id == 60 || id == 20) {
            MakePoofParticles();
        } else if (id == kEntityEventSwing) {
            // The server's melee whack (MC's Animate packet stand-in) — run
            // the local swing clock. Client side, so Swing() won't rebroadcast.
            Swing();
        } else {
            Entity::HandleEntityEvent(id);
        }
    }

    void LivingEntity::OnEquippedItemBroken(const ItemStack& broken, EquipmentSlot slot) {
        // MC onEquippedItemBroken: broadcastEntityEvent(entityEventForEquipment-
        // Break(slot)); stopLocationBasedEffects has nothing to undo here — an
        // entity's attribute modifiers are not built from its equipment.
        (void)broken;
        if (m_level && !m_level->IsClientSide()) {
            m_level->BroadcastEntityEvent(*this, EntityEventForEquipmentBreak(slot));
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
            // MC LivingEntity.remove(KILLED) → triggerOnDeathMobEffects.
            TriggerOnDeathMobEffects(RemovalReason::Killed);
            Remove(RemovalReason::Killed);
        }
    }

} // namespace Game
