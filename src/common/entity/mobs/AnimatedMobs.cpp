// File: src/common/entity/mobs/AnimatedMobs.cpp
#include "common/entity/mobs/AnimatedMobs.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/goals/BeeGoals.hpp"
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/ai/goals/HappyGhastGoals.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/brain/BreezeAi.hpp"
#include "common/entity/ai/brain/CamelAi.hpp"
#include "common/entity/ai/brain/CopperGolemAi.hpp"
#include "common/entity/ai/brain/CreakingAi.hpp"
#include "common/entity/ai/brain/GoatAi.hpp"
#include "common/entity/ai/brain/HoglinAi.hpp"
#include "common/entity/ai/brain/SnifferAi.hpp"
#include "common/entity/ai/brain/TadpoleAi.hpp"
#include "common/entity/ai/brain/FrogAi.hpp"
#include "common/entity/ai/brain/WardenAi.hpp"
#include "common/entity/ai/brain/AllayAi.hpp"
#include "common/entity/ai/brain/ArmadilloAi.hpp"
#include "common/entity/ai/brain/AxolotlAi.hpp"
#include "common/entity/ai/brain/HappyGhastAi.hpp"
#include "common/entity/ai/brain/PiglinAi.hpp"
#include "common/entity/ai/brain/PiglinBruteAi.hpp"
#include "common/entity/ai/brain/ZoglinAi.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ══ Frog ═══════════════════════════════════════════════════════════════

    Frog::Frog(EntityLevel* level) : GenericAnimal(EntityTypeId::Frog, level) {
        // NO GOALS. MC's Frog never overrides registerGoals, so it has none —
        // its entire behaviour is the brain. GenericAnimal's constructor has
        // already registered the shared animal set, so it is cleared here.
        //
        // This is not tidiness. MC's Croak is gated on WALK_TARGET being
        // ABSENT, and that only means "not walking" if every movement goes
        // through a walk target. With the goal stroll still running the frog
        // croaked while walking, and MoveToTargetSink fought the stroll goal
        // for the navigation.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Frog.createNavigation returns a FrogPathNavigation — amphibious,
        // so water costs nothing to swim through and land costs 6. On the
        // ground navigation the frog could not path THROUGH water at all: the
        // SWIM activity would pick targets the pathfinder refused to reach and
        // the frog would sit at the water's edge looking broken.
        m_navigation = std::make_unique<FrogPathNavigation>(this, level);

        m_brain = std::make_unique<Brain>();
        FrogAi::InitBrain(*this, *m_brain);
        FrogAi::InitMemories(*this);
    }

    void Frog::UpdateBrainActivity() {
        // MC Frog.customServerAiStep: tick the brain, then re-pick the
        // activity. Mob::ServerAiStep does the first half.
        FrogAi::UpdateActivity(*this);
    }

    void Frog::Tick() {

        // MC Frog.tick runs this BEFORE super.tick(), and it is purely local —
        // no server state is involved, so every client decides for itself.
        if (m_level && m_level->IsClientSide()) {
            Anim(MobAnim::SwimIdle).AnimateWhen(
                IsInWater() && !walkAnimation.IsMoving(), tickCount);
        }
        GenericAnimal::Tick();
    }

    void Frog::OnPoseUpdated() {
        const Pose pose = GetPose();
        // MC starts (not startIfStopped) each of these: the pose only changes
        // on the transition, so a restart is exactly one clip from the top.
        if (pose == Pose::LongJumping) Anim(MobAnim::Jump).Start(tickCount);
        else                           Anim(MobAnim::Jump).Stop();

        if (pose == Pose::Croaking)    Anim(MobAnim::Croak).Start(tickCount);
        else                           Anim(MobAnim::Croak).Stop();

        if (pose == Pose::UsingTongue) Anim(MobAnim::Tongue).Start(tickCount);
        else                           Anim(MobAnim::Tongue).Stop();
    }

    void Frog::UpdateWalkAnimation(float distance) {
        // MC Frog.updateWalkAnimation.
        const float target = Anim(MobAnim::Jump).IsStarted()
                                 ? 0.0f
                                 : std::min(distance * 25.0f, 1.0f);
        walkAnimation.Update(target, 0.4f, IsBaby() ? 3.0f : 1.0f);
    }

    // ══ Camel ══════════════════════════════════════════════════════════════

    Camel::Camel(EntityLevel* level, EntityTypeId type) : GenericAnimal(type, level) {
        // NO GOALS — MC's Camel has a brain and never registers any.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        CamelAi::InitBrain(*this, *m_brain);

        // MC finalizeSpawn calls resetLastPoseChangeTickToFullStand, so a fresh
        // camel is already fully stood up rather than mid-transition.
        StandUpInstantly();
    }

    void Camel::UpdateBrainActivity() { CamelAi::UpdateActivity(*this); }

    int64_t Camel::GetPoseTime() const {
        const int64_t now = m_level ? m_level->GetGameTime() : 0;
        return now - (m_lastPoseChangeTick < 0 ? -m_lastPoseChangeTick : m_lastPoseChangeTick);
    }

    void Camel::ResetLastPoseChangeTick(int64_t syncedPoseTickTime) {
        m_lastPoseChangeTick = syncedPoseTickTime;
    }

    void Camel::SitDown() {
        if (IsCamelSitting()) return;
        SetPose(Pose::Sitting);
        // NEGATIVE while sitting — that sign IS the sitting flag in MC.
        ResetLastPoseChangeTick(-(m_level ? m_level->GetGameTime() : 0));
    }

    void Camel::StandUp() {
        if (!IsCamelSitting()) return;
        SetPose(Pose::Standing);
        ResetLastPoseChangeTick(m_level ? m_level->GetGameTime() : 0);
    }

    void Camel::StandUpInstantly() {
        SetPose(Pose::Standing);
        // MC backdates the change past the whole stand-up so the camel is not
        // considered "in transition" at all.
        const int64_t now = m_level ? m_level->GetGameTime() : 0;
        ResetLastPoseChangeTick(std::max<int64_t>(0, now - kStandUpDuration - 1));
    }

    void Camel::OnPoseUpdated() {
        // The CLIENT learns the pose from the wire and recomputes the tick from
        // it — see the note on m_lastPoseChangeTick.
        if (!m_level || !m_level->IsClientSide()) return;
        const int64_t now = m_level->GetGameTime();
        if (GetPose() == Pose::Sitting) {
            if (!IsCamelSitting()) ResetLastPoseChangeTick(-now);
        } else if (IsCamelSitting()) {
            ResetLastPoseChangeTick(now);
        }
    }

    void Camel::Tick() {
        GenericAnimal::Tick();

        // MC Camel.tick's dash bookkeeping, both sides: the dash flag clears
        // once the camel is back on the ground (or in liquid) and the cooldown
        // has run 5 ticks (55 → below 50), and the cooldown itself counts down
        // wherever it was armed.
        if (IsDashing() && m_dashCooldown < 50
            && (onGround || IsInLiquid())) {
            SetDashing(false);
        }
        if (m_dashCooldown > 0) {
            --m_dashCooldown;
            // MC plays CAMEL_DASH_READY at zero; no sound system yet.
        }

        // MC Camel.tick: a sitting camel that ends up in water stands straight
        // back up, because the sitting hitbox would drown it.
        if (m_level && !m_level->IsClientSide() && IsCamelSitting() && IsInWater()) {
            StandUpInstantly();
        }
    }

    void Camel::SetAnimStateByte(uint8_t v) {
        // MC Camel.onSyncedDataUpdated's DASH branch: the client re-arms its
        // local 55-tick cooldown when the flag turns on, so its auto-clear
        // logic in tick() matches the server's timing.
        const bool dashing = (v & 1) != 0;
        if (dashing && !m_dashing && m_dashCooldown == 0) {
            m_dashCooldown = kDashCooldownTicks;
        }
        m_dashing = dashing;
    }

    void Camel::SetupAnimationStates() {
        // MC Camel.setupAnimationStates, now complete: the sit clips are live
        // because RandomSitting actually sits the camel down.
        if (m_idleAnimationTimeout <= 0) {
            m_idleAnimationTimeout =
                (m_level ? m_level->Random().NextInt(40) : 0) + 80;
            Anim(MobAnim::Idle).Start(tickCount);
        } else {
            --m_idleAnimationTimeout;
        }

        if (IsCamelVisuallySitting()) {
            Anim(MobAnim::SitUp).Stop();
            Anim(MobAnim::Dash).Stop();
            if (IsVisuallySittingDown()) {
                Anim(MobAnim::Sit).StartIfStopped(tickCount);
                Anim(MobAnim::SitPose).Stop();
            } else {
                Anim(MobAnim::Sit).Stop();
                Anim(MobAnim::SitPose).StartIfStopped(tickCount);
            }
        } else {
            Anim(MobAnim::Sit).Stop();
            Anim(MobAnim::SitPose).Stop();
            // MC's line verbatim: the dash flag rides the anim-state byte, so
            // this fires the moment the server's setDashing(true) — a rider's
            // charged jump, once riding exists — reaches this client.
            Anim(MobAnim::Dash).AnimateWhen(IsDashing(), tickCount);
            Anim(MobAnim::SitUp).AnimateWhen(IsInPoseTransition() && GetPoseTime() >= 0,
                                             tickCount);
        }
    }

    // ══ Bat ════════════════════════════════════════════════════════════════

    namespace {

        // MC BlockState.isRedstoneConductor, as near as this engine gets: a
        // full-cube collision shape. MC additionally excludes signal sources
        // (a redstone block is not a conductor), which changes nothing here —
        // a bat's only use of it is "can I hang from this ceiling".
        bool IsCeilingBlock(const IBlockAccess* blocks, int x, int y, int z) {
            return blocks && blocks->IsBlockSolid(x, y, z);
        }

        // MC Level.getMinY for the overworld. A bat that picked a target
        // below the world would never reach it and would stop steering.
        constexpr int kBatMinY = -64;

        bool IsEmptyBlock(const IBlockAccess* blocks, int x, int y, int z) {
            return !blocks || blocks->GetBlock(x, y, z) == BlockID::Air;
        }

    } // namespace

    Bat::Bat(EntityLevel* level) : GenericMob(EntityTypeId::Bat, level) {
        // MC's Bat does not override registerGoals at all — its entire
        // behaviour is customServerAiStep below. GenericMob's default set
        // (float, look-at-player, look-around) would fight it: the look goals
        // write yHeadRot every tick while the flight code is steering yRot from
        // the velocity, so the bat would fly one way and face another.
        m_goalSelector.Clear();

        // MC's Bat constructor: `this.setResting(true)`. A bat spawns hanging.
        m_resting = true;
    }

    void Bat::Tick() {
        GenericMob::Tick();

        if (m_resting) {
            // MC pins a resting bat to the ceiling: no motion at all, and the
            // body hung from the block above rather than standing on the floor.
            velocity = glm::dvec3(0.0);
            position.y = std::floor(position.y) + 1.0
                       - static_cast<double>(GetBbHeight());
        } else {
            // MC damps the VERTICAL component only, which is what turns the
            // 0.7 upward push into a flutter instead of a climb.
            velocity.y *= 0.6;
        }
    }

    void Bat::SetupAnimationStates() {
        // MC Bat.setupAnimationStates.
        if (m_resting) {
            Anim(MobAnim::Fly).Stop();
            Anim(MobAnim::Rest).StartIfStopped(tickCount);
        } else {
            Anim(MobAnim::Rest).Stop();
            Anim(MobAnim::Fly).StartIfStopped(tickCount);
        }
    }

    void Bat::CustomServerAiStep() {
        // MC Bat.customServerAiStep, transcribed. This is the whole of a bat's
        // AI — no navigation, no goals, just a wandering target position it
        // steers toward — which is why it ports directly where the brain mobs
        // do not.
        if (!m_level) return;
        const IBlockAccess* blocks = m_level->Blocks();

        const glm::ivec3 pos = BlockPosition();
        const glm::ivec3 above(pos.x, pos.y + 1, pos.z);

        if (m_resting) {
            if (IsCeilingBlock(blocks, above.x, above.y, above.z)) {
                if (m_level->Random().NextInt(200) == 0) {
                    yHeadRot = static_cast<float>(m_level->Random().NextInt(360));
                }
                // MC BAT_RESTING_TARGETING is `forNonCombat().range(4)`.
                if (m_level->GetNearestPlayer(position.x, position.y, position.z,
                                              4.0) != nullptr) {
                    m_resting = false;
                }
            } else {
                // The block it was hanging from is gone.
                m_resting = false;
            }
            return;
        }

        if (m_hasTarget
            && (!IsEmptyBlock(blocks, m_targetPosition.x, m_targetPosition.y,
                              m_targetPosition.z)
                || m_targetPosition.y <= kBatMinY)) {
            m_hasTarget = false;
        }

        JavaRandom& rnd = m_level->Random();
        const glm::dvec3 targetCentre(
            static_cast<double>(m_targetPosition.x) + 0.5,
            static_cast<double>(m_targetPosition.y),
            static_cast<double>(m_targetPosition.z) + 0.5);
        const double dxT = targetCentre.x - position.x;
        const double dzT = targetCentre.z - position.z;
        const double dyT = targetCentre.y - position.y;

        if (!m_hasTarget || rnd.NextInt(30) == 0
            || (dxT * dxT + dyT * dyT + dzT * dzT) < 4.0) {
            // MC draws each axis from two independent nextInt(7)s, so the
            // offset is triangular rather than uniform — bats hover near where
            // they are far more often than they dart 6 blocks away.
            m_targetPosition = glm::ivec3(
                static_cast<int>(std::floor(position.x)) + rnd.NextInt(7) - rnd.NextInt(7),
                static_cast<int>(std::floor(position.y)) + rnd.NextInt(6) - 2,
                static_cast<int>(std::floor(position.z)) + rnd.NextInt(7) - rnd.NextInt(7));
            m_hasTarget = true;
        }

        const double dx = static_cast<double>(m_targetPosition.x) + 0.5 - position.x;
        const double dy = static_cast<double>(m_targetPosition.y) + 0.1 - position.y;
        const double dz = static_cast<double>(m_targetPosition.z) + 0.5 - position.z;

        const auto signum = [](double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); };
        velocity.x += (signum(dx) * 0.5 - velocity.x) * 0.1;
        velocity.y += (signum(dy) * 0.7 - velocity.y) * 0.1;
        velocity.z += (signum(dz) * 0.5 - velocity.z) * 0.1;

        const float wanted = static_cast<float>(
            std::atan2(velocity.z, velocity.x) * (180.0 / 3.14159265358979323846)) - 90.0f;
        yRot += Mth::WrapDegrees(wanted - yRot);
        zza = 0.5f;

        if (rnd.NextInt(100) == 0
            && IsCeilingBlock(blocks, above.x, above.y, above.z)) {
            m_resting = true;
        }
    }

    // ══ Tadpole / Goat / Hoglin ═══════════════════════════════════════════

    Tadpole::Tadpole(EntityLevel* level) : GenericPathfinderMob(EntityTypeId::Tadpole, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        TadpoleAi::InitBrain(*this, *m_brain);
    }
    void Tadpole::UpdateBrainActivity() { TadpoleAi::UpdateActivity(*this); }

    void Tadpole::BaseTick() {
        // MC WaterAnimal.baseTick (Tadpole extends AbstractFish): a stranded
        // tadpole suffocates like a beached fish.
        const int airSupply = GetAirSupply();
        GenericPathfinderMob::BaseTick();
        HandleWaterAnimalAirSupply(*this, airSupply);
    }

    Goat::Goat(EntityLevel* level) : GenericAnimal(EntityTypeId::Goat, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        GoatAi::InitBrain(*this, *m_brain);
        GoatAi::InitMemories(*this);
    }
    void Goat::UpdateBrainActivity() { GoatAi::UpdateActivity(*this); }

    Hoglin::Hoglin(EntityLevel* level) : GenericAnimal(EntityTypeId::Hoglin, level) {
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        HoglinAi::InitBrain(*this, *m_brain);
    }
    void Hoglin::UpdateBrainActivity() { HoglinAi::UpdateActivity(*this); }

    bool Hoglin::DoHurtTarget(Entity& target) {
        // MC Hoglin.doHurtTarget: only living targets; arm the headbutt clock
        // and broadcast event 4 BEFORE the hit lands, so the animation starts
        // on the same tick. The HOGLIN_ATTACK sound waits on the sound
        // system; HoglinAi.onHitTarget (the pack-retaliation memory) is not
        // ported; HoglinBase.hurtAndThrowTarget's damage + fling is covered
        // by the base hit — ATTACK_KNOCKBACK 1.0 from the def rides the
        // base's extra knockback (the extra vertical toss is not modelled).
        if (dynamic_cast<LivingEntity*>(&target) == nullptr) return false;
        m_attackAnimationRemainingTicks = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        return GenericAnimal::DoHurtTarget(target);
    }

    void Hoglin::AiStep() {
        // MC Hoglin.aiStep: the clock counts down BEFORE super — both sides,
        // which is what animates a remote hoglin's headbutt.
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }
        GenericAnimal::AiStep();
    }

    void Hoglin::HandleEntityEvent(uint8_t id) {
        // MC Hoglin.handleEntityEvent(4) — restart the headbutt clock (the
        // attack sound waits on the sound system).
        if (id == 4) {
            m_attackAnimationRemainingTicks = 10;
        } else {
            GenericAnimal::HandleEntityEvent(id);
        }
    }

    // ── Zoglin ─────────────────────────────────────────────────────────────

    Zoglin::Zoglin(EntityLevel* level) : GenericMonster(EntityTypeId::Zoglin, level) {
        // NO GOALS — MC's Zoglin never registers any; its whole behaviour is
        // the brain. GenericMonster's constructor already registered the
        // def-driven monster set, so it is cleared here.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        ZoglinAi::InitBrain(*this, *m_brain);
    }

    void Zoglin::UpdateBrainActivity() { ZoglinAi::UpdateActivity(*this); }

    void Zoglin::SetBaby(bool baby) {
        // MC Zoglin.setBaby: the synched flag (the wire's shared baby bit
        // here), and on the server the attack-damage drop to BABY_ATTACK_DAMAGE.
        if (m_baby == baby) return;
        m_baby = baby;
        if (baby && m_level && !m_level->IsClientSide()) {
            m_attributes.SetBaseValue(Attribute::AttackDamage, 0.5);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Zoglin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Zoglin.finalizeSpawn — a flat 20% baby roll, no group token.
        if (m_level && m_level->Random().NextFloat() < 0.2f) {
            SetBaby(true);
        }
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    bool Zoglin::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        // MC Zoglin.hurtServer: retarget onto the attacker unless the current
        // target is much closer (BehaviorUtils.isOtherTargetMuchFurtherAway-
        // ThanCurrentAttackTarget, threshold 4).
        if (hurt && m_level && !m_level->IsClientSide()) {
            auto* living = dynamic_cast<LivingEntity*>(attacker);
            if (living && CanAttack(*living)) {
                bool muchFurther = false;
                if (const Brain* brain = GetBrain()) {
                    auto* current = dynamic_cast<LivingEntity*>(
                        brain->GetEntity(MemoryModule::AttackTarget));
                    if (current) {
                        // MC compares SQUARED distances plus threshold² — the
                        // exact expression, asymmetric as it is.
                        muchFurther = DistanceToSqr(*living)
                                    > DistanceToSqr(*current) + 4.0 * 4.0;
                    }
                }
                if (!muchFurther) {
                    ZoglinAi::SetAttackTarget(*this, *living);
                }
            }
        }
        return hurt;
    }

    bool Zoglin::DoHurtTarget(Entity& target) {
        // MC Zoglin.doHurtTarget, the Hoglin twin: only living targets, arm
        // the clock, broadcast event 4, then the hit. The ZOGLIN_ATTACK
        // sound waits on the sound system; hurtAndThrowTarget's fling is
        // covered by the base's ATTACK_KNOCKBACK (1.0 from the def).
        if (dynamic_cast<LivingEntity*>(&target) == nullptr) return false;
        m_attackAnimationRemainingTicks = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        return GenericMonster::DoHurtTarget(target);
    }

    void Zoglin::AiStep() {
        // MC Zoglin.aiStep: the clock counts down BEFORE super, both sides.
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }
        GenericMonster::AiStep();
    }

    void Zoglin::HandleEntityEvent(uint8_t id) {
        // MC Zoglin.handleEntityEvent(4) — restart the headbutt clock.
        if (id == 4) {
            m_attackAnimationRemainingTicks = 10;
        } else {
            GenericMonster::HandleEntityEvent(id);
        }
    }

    // ── Piglin / PiglinBrute ───────────────────────────────────────────────

    Piglin::Piglin(EntityLevel* level) : GenericMonster(EntityTypeId::Piglin, level) {
        // NO GOALS — MC's Piglin never registers any; the brain is the whole
        // behaviour. setCanPickUpLoot(true) and the door-opening ability are
        // skipped (items / door interaction); the fire maluses are MC
        // AbstractPiglin's.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        PiglinAi::InitBrain(*this, *m_brain);
    }

    void Piglin::UpdateBrainActivity() { PiglinAi::UpdateActivity(*this); }

    void Piglin::SetBaby(bool baby) {
        // MC Piglin.setBaby — SPEED_MODIFIER_BABY: +20% ADD_MULTIPLIED_BASE.
        if (m_baby == baby) return;
        m_baby = baby;
        if (baby) {
            m_attributes.AddModifier(Attribute::MovementSpeed,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::BabySpeedBoost), 0.2,
                                   AttributeOperation::AddMultipliedBase });
        } else {
            m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                        ModifierId::BabySpeedBoost);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Piglin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Piglin.finalizeSpawn — 20% baby (the adult's spawn weapon and
        // the 10%-per-piece gold armor rolls are skipped: no equipment
        // system). MC exempts STRUCTURE spawns; structures do not spawn mobs
        // here.
        if (m_level && m_level->Random().NextFloat() < 0.2f) {
            SetBaby(true);
        }
        PiglinAi::InitMemories(*this);
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    bool Piglin::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        if (hurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                PiglinAi::WasHurtBy(*m_level, *this, *living);
            }
        }
        return hurt;
    }

    void Piglin::CustomServerAiStep() {
        // MC AbstractPiglin.customServerAiStep — the zombification clock.
        // This engine's single dimension IS the overworld, where MC's
        // PIGLINS_ZOMBIFY attribute is true, so an unprotected piglin
        // converts after CONVERSION_TIME (300) ticks exactly as one brought
        // through a portal does.
        if (!m_level || m_level->IsClientSide()) return;
        const bool converting = !IsImmuneToZombification() && !IsNoAi();
        m_timeInOverworld = converting ? m_timeInOverworld + 1 : 0;
        if (m_timeInOverworld > 300) {
            // MC finishConversion → ZOMBIFIED_PIGLIN. The 200-tick nausea on
            // the convert is skipped (no nausea effect); the conversion-shake
            // visual is skipped like the zombie→drowned one; cancelAdmiring
            // and the inventory drop are items-system work.
            auto zombified = std::make_unique<ZombifiedPiglin>(m_level);
            CopyConversionState(*zombified);
            zombified->SetBaby(IsBaby());
            FinishConversion(std::move(zombified));
        }
    }

    PiglinBrute::PiglinBrute(EntityLevel* level)
        : GenericMonster(EntityTypeId::PiglinBrute, level) {
        // NO GOALS — the brain is the whole behaviour. Golden-axe spawn
        // equipment skipped (no equipment system).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        PiglinBruteAi::InitBrain(*this, *m_brain);
    }

    void PiglinBrute::UpdateBrainActivity() { PiglinBruteAi::UpdateActivity(*this); }

    std::shared_ptr<SpawnGroupData>
    PiglinBrute::FinalizeSpawn(SpawnReason reason,
                               std::shared_ptr<SpawnGroupData> groupData) {
        PiglinBruteAi::InitMemories(*this);
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    bool PiglinBrute::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        if (hurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                PiglinBruteAi::WasHurtBy(*m_level, *this, *living);
            }
        }
        return hurt;
    }

    void PiglinBrute::CustomServerAiStep() {
        // MC AbstractPiglin.customServerAiStep — see Piglin::CustomServerAiStep.
        if (!m_level || m_level->IsClientSide()) return;
        const bool converting = !IsImmuneToZombification() && !IsNoAi();
        m_timeInOverworld = converting ? m_timeInOverworld + 1 : 0;
        if (m_timeInOverworld > 300) {
            auto zombified = std::make_unique<ZombifiedPiglin>(m_level);
            CopyConversionState(*zombified);
            FinishConversion(std::move(zombified));
        }
    }

    // ── Axolotl ────────────────────────────────────────────────────────────

    namespace {

        // MC Axolotl.AxolotlMoveControl / AxolotlLookControl — the stock
        // smooth-swimming controls, frozen while playing dead.
        class AxolotlMoveControl : public SmoothSwimmingMoveControl {
        public:
            explicit AxolotlMoveControl(Axolotl* axolotl)
                : SmoothSwimmingMoveControl(axolotl, 85, 10, 0.1f, 0.5f, false),
                  m_axolotl(axolotl) {}
            void Tick() override {
                if (!m_axolotl->IsPlayingDead()) SmoothSwimmingMoveControl::Tick();
            }
        private:
            Axolotl* m_axolotl;
        };

        class AxolotlLookControl : public SmoothSwimmingLookControl {
        public:
            explicit AxolotlLookControl(Axolotl* axolotl)
                : SmoothSwimmingLookControl(axolotl, 20), m_axolotl(axolotl) {}
            void Tick() override {
                if (!m_axolotl->IsPlayingDead()) SmoothSwimmingLookControl::Tick();
            }
        private:
            Axolotl* m_axolotl;
        };

        // MC Axolotl.AxolotlGroupData — the pack token: two common variants
        // rolled once, and a member counter (AgeableMobGroupData's) that makes
        // the third axolotl onward spawn as a baby.
        struct AxolotlGroupData : SpawnGroupData {
            Axolotl::Variant types[2];
            int groupSize = 0;
        };

        // MC Axolotl.Variant.getCommonSpawnVariant — LUCY/WILD/GOLD/CYAN are
        // common; BLUE is the 1-in-1200 breeding rare.
        Axolotl::Variant CommonVariant(JavaRandom& rng) {
            return static_cast<Axolotl::Variant>(rng.NextInt(4));
        }

    } // namespace

    // MC EasingType.IN_OUT_SINE over the animator's 0..1 ramp.
    float Axolotl::BinaryAnimator::Factor(float partialTick) const {
        const float t = (static_cast<float>(ticksOld)
                         + (static_cast<float>(ticks) - static_cast<float>(ticksOld))
                               * partialTick)
                        / static_cast<float>(length);
        return -(std::cos(3.14159265358979f * t) - 1.0f) / 2.0f;
    }

    Axolotl::Axolotl(EntityLevel* level) : GenericAnimal(EntityTypeId::Axolotl, level) {
        // NO GOALS — MC's Axolotl never registers any; the brain is the whole
        // behaviour. The def's amphibious navigation stands (MC
        // createNavigation returns AmphibiousPathNavigation).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<AxolotlMoveControl>(this));
        SetLookControl(std::make_unique<AxolotlLookControl>(this));
        // The swim control owns the on-land slowdown (outsideWater 0.5), the
        // same division of labour ApplyLocomotion documents.
        SetLandSpeedFactor(1.0f);

        m_brain = std::make_unique<Brain>();
        AxolotlAi::InitBrain(*this, *m_brain);
    }

    void Axolotl::TickAnimations() {
        // MC Axolotl.tickAnimations — client-side only, one exclusive state a
        // tick, each animator easing toward its own state.
        enum class AnimState { PlayingDead, InWater, OnGround, InAir };
        AnimState s;
        if (IsPlayingDead())  s = AnimState::PlayingDead;
        else if (IsInWater()) s = AnimState::InWater;
        else if (onGround)    s = AnimState::OnGround;
        else                  s = AnimState::InAir;

        m_playingDeadAnimator.TickAnim(s == AnimState::PlayingDead);
        m_inWaterAnimator.TickAnim(s == AnimState::InWater);
        m_onGroundAnimator.TickAnim(s == AnimState::OnGround);
        const bool moving = walkAnimation.IsMoving()
                         || xRot != xRotO || yRot != yRotO;
        m_movingAnimator.TickAnim(moving);
    }

    void Axolotl::BaseTick() {
        // MC Axolotl.baseTick: capture the PRE-tick air (super's own block
        // would top a beached axolotl back up), run super, then apply the
        // axolotl's inverted air rule on the server.
        const int airSupply = GetAirSupply();
        GenericAnimal::BaseTick();

        if (m_level && !m_level->IsClientSide()) {
            // MC Axolotl.handleAirSupply. MC exempts rain (isInWaterOrRain);
            // no weather system here, so water is the whole test — the same
            // reduction World::IsRainingAt documents.
            if (IsAlive() && !IsInWater()) {
                SetAirSupply(airSupply - 1);
                if (ShouldTakeDrowningDamage()) {
                    SetAirSupply(0);
                    // MC's dryOut damage source, 2.0 per tick once dry.
                    Hurt(MobDamageSource::Drown, 2.0f, nullptr);
                }
            } else {
                SetAirSupply(GetMaxAirSupply());
            }
        }

        if (m_level && m_level->IsClientSide()) {
            // MC 26.2: `if (isBaby()) tickBabyAnimations(); else
            // tickAdultAnimations();`. Both run here so the classic baby
            // mesh (the adult's animators) and the remodel (the keyframe
            // states) are each driven whichever look is drawn.
            TickAnimations();
            if (IsBaby()) TickBabyAnimations();
        }
    }

    void Axolotl::TickBabyAnimations() {
        // MC Axolotl.tickBabyAnimations + soloAnimation, verbatim: the
        // chosen state startIfStopped, every other one stopped.
        const bool inWater = IsInWater();
        const bool moving  = walkAnimation.IsMoving() || xRot != xRotO || yRot != yRotO;
        MobAnim solo;
        if (IsPlayingDead())          solo = MobAnim::PlayDead;
        else if (moving) {
            if (inWater && !onGround)      solo = MobAnim::Swim;
            else if (!inWater && onGround) solo = MobAnim::Walk;
            else                           solo = MobAnim::WalkUnderWater;
        }
        else if (inWater && !onGround) solo = MobAnim::IdleUnderWater;
        else if (inWater && onGround)  solo = MobAnim::IdleUnderWaterOnGround;
        else                           solo = MobAnim::IdleOnGround;

        static constexpr MobAnim kAll[] = {
            MobAnim::Swim, MobAnim::Walk, MobAnim::WalkUnderWater,
            MobAnim::IdleUnderWater, MobAnim::IdleUnderWaterOnGround,
            MobAnim::IdleOnGround, MobAnim::PlayDead,
        };
        for (MobAnim a : kAll) {
            if (a == solo) Anim(a).StartIfStopped(tickCount);
            else           Anim(a).Stop();
        }
    }

    void Axolotl::UpdateBrainActivity() {
        AxolotlAi::UpdateActivity(*this);
        // MC customServerAiStep's tail: mirror PLAY_DEAD_TICKS into the
        // synched playing-dead flag every tick.
        if (const Brain* brain = GetBrain()) {
            const std::optional<int> ticks = brain->GetInt(MemoryModule::PlayDeadTicks);
            SetPlayingDead(ticks.has_value() && *ticks > 0);
        }
    }

    bool Axolotl::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Axolotl.hurtServer — the play-dead roll runs BEFORE the damage
        // lands, against the pre-hit health: 1-in-3, and only when the hit is
        // meaningful (damage beats a 0..2 roll, or already below half
        // health), non-lethal, from an entity, in water, and not already
        // playing dead.
        if (m_level && !m_level->IsClientSide()) {
            JavaRandom& rng = m_level->Random();
            const float health = GetHealth();
            if (rng.NextInt(3) == 0
                && (static_cast<float>(rng.NextInt(3)) < amount
                    || health / GetMaxHealth() < 0.5f)
                && amount < health && IsInWater() && attacker != nullptr
                && !IsPlayingDead()) {
                if (Brain* brain = GetBrain()) {
                    brain->SetMemory(MemoryModule::PlayDeadTicks, kTotalPlayDeadTime);
                }
            }
        }
        return GenericAnimal::Hurt(source, amount, attacker);
    }

    std::shared_ptr<SpawnGroupData>
    Axolotl::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC's EntitySpawnReason.BUCKET early-out is unreachable here — no
        // bucket system, so no such spawn reason exists.
        if (!m_level) return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
        JavaRandom& rng = m_level->Random();

        bool isBaby = false;
        auto data = std::dynamic_pointer_cast<AxolotlGroupData>(groupData);
        if (data) {
            // MC: the third pack member onward spawns as a baby.
            if (data->groupSize >= 2) isBaby = true;
        } else {
            data = std::make_shared<AxolotlGroupData>();
            data->types[0] = CommonVariant(rng);
            data->types[1] = CommonVariant(rng);
            groupData = data;
        }
        SetVariant(data->types[rng.NextInt(2)]);
        if (isBaby) SetAge(-24000);
        // MC AgeableMob.finalizeSpawn's member counter, run by super there.
        ++data->groupSize;

        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    void Axolotl::SpawnChildFromBreeding(Animal& partner) {
        // Stash the partner for CreateBaby's variant coin flip — MC's
        // getBreedOffspring receives the partner directly.
        m_breedPartner = &partner;
        GenericAnimal::SpawnChildFromBreeding(partner);
        m_breedPartner = nullptr;
    }

    std::unique_ptr<Animal> Axolotl::CreateBaby() {
        auto baby = std::make_unique<Axolotl>(m_level);
        // MC getBreedOffspring: 1-in-1200 the rare variant (blue is the only
        // one), else a coin flip between the two parents.
        Variant v = GetVariant();
        if (m_level) {
            JavaRandom& rng = m_level->Random();
            if (rng.NextInt(1200) == 0) {
                v = Variant::Blue;
            } else if (!rng.NextBool()) {
                // MC: nextBoolean ? own variant : partner's.
                if (auto* p = dynamic_cast<Axolotl*>(m_breedPartner)) {
                    v = p->GetVariant();
                }
            }
        }
        baby->SetVariant(v);
        return baby;
    }

    void Axolotl::OnStopAttacking(EntityLevel& level, Axolotl& axolotl,
                                  LivingEntity& target) {
        // MC Axolotl.onStopAttacking: if the target died and the killing blow
        // traced to a player within 20 blocks, buff that player. MC reads the
        // target's lastDamageSource entity; the port's equivalent record is
        // lastHurtByMob.
        if (!target.IsDeadOrDying()) return;
        auto* player = dynamic_cast<LivingEntity*>(target.GetLastHurtByMob());
        if (!player || !player->IsPlayer()) return;

        std::vector<LivingEntity*> players;
        level.GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p == player
                && axolotl.DistanceToSqr(*p) <= 20.0 * 20.0) {
                axolotl.ApplySupportingEffects(*p);
                return;
            }
        }
    }

    void Axolotl::ApplySupportingEffects(LivingEntity& player) {
        // MC Axolotl.applySupportingEffects: top regeneration up by 100 ticks
        // to a 2400-tick cap (skipping only a longer-running one), and clear
        // mining fatigue.
        const MobEffectInstance* regen = player.GetEffect(MobEffectId::Regeneration);
        // MC endsWithin(2399): a finite regen with 2399 ticks or fewer left.
        if (!regen || (regen->duration >= 0 && regen->duration <= 2399)) {
            const int previous = regen ? regen->duration : 0;
            const int duration = std::min(2400, 100 + previous);
            player.AddEffect(MobEffectInstance(MobEffectId::Regeneration, duration, 0),
                             this);
        }
        player.RemoveEffect(MobEffectId::MiningFatigue);
    }

    // ── Bee ────────────────────────────────────────────────────────────────

    Bee::Bee(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Bee, level), NeutralMob(this) {
        // The def-driven goal set (flying stroll, float, look goals, tempt +
        // breed from BEE_FOOD) is registered by the base constructor. The
        // hive/flower/crop goals wait on their systems (see the header); the
        // combat trio is MC's own, priority for priority
        // (Bee.registerGoals):
        m_goalSelector.AddGoal(0, std::make_unique<BeeAttackGoal>(this, 1.4, true));
        auto hurtBy = std::make_unique<BeeHurtByOtherGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<BeeBecomeAngryTargetGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/true));

        // The flower/crop layer (Bee.registerGoals priorities; the hive goals
        // stay with the hive block-entity system — see BeeGoals.hpp). The
        // shared flower state carries MC's ctor cooldown roll.
        m_flowerState = std::make_shared<BeeFlowerState>();
        if (m_level) {
            m_flowerState->remainingCooldownBeforeLocatingNewFlower =
                m_level->Random().NextInt(20, 60);
        }
        m_goalSelector.AddGoal(3, std::make_unique<ValidateFlowerGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(4, std::make_unique<BeePollinateGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(7, std::make_unique<BeeGrowCropGoal>(this, m_flowerState));
        m_goalSelector.AddGoal(8, std::make_unique<BeeWanderGoal>(this, m_flowerState));
    }

    uint8_t Bee::GetAnimStateByte() const {
        return static_cast<uint8_t>(
            (m_rolling ? 1 : 0) | (m_hasStung ? 2 : 0) |
            (((m_flowerState && m_flowerState->hasNectar) ||
              m_clientHasNectar) ? 4 : 0));
    }

    void Bee::SetAnimStateByte(uint8_t v) {
        m_rolling = (v & 1) != 0;
        m_hasStung = (v & 2) != 0;
        m_clientHasNectar = (v & 4) != 0;
    }

    // ── Bee save/load seam ─────────────────────────────────────────────────
    //
    // m_flowerState is shared with the goals and is null until RegisterGoals
    // runs, so every one of these tolerates its absence rather than asserting:
    // the load path can reach a bee whose goals have not been built yet.

    bool Bee::HasNectar() const {
        return m_flowerState ? m_flowerState->hasNectar : m_clientHasNectar;
    }

    void Bee::SetHasNectar(bool v) {
        if (m_flowerState) m_flowerState->hasNectar = v;
        m_clientHasNectar = v;
    }

    bool Bee::HasSavedFlowerPos() const {
        return m_flowerState && m_flowerState->hasSavedFlowerPos;
    }

    glm::ivec3 Bee::GetSavedFlowerPos() const {
        return m_flowerState ? m_flowerState->savedFlowerPos : glm::ivec3(0);
    }

    void Bee::SetSavedFlowerPos(const glm::ivec3& pos) {
        if (!m_flowerState) return;
        m_flowerState->savedFlowerPos = pos;
        m_flowerState->hasSavedFlowerPos = true;
    }

    int Bee::GetTicksWithoutNectar() const {
        return m_flowerState ? m_flowerState->ticksWithoutNectarSinceExitingHive : 0;
    }

    void Bee::SetTicksWithoutNectar(int ticks) {
        if (m_flowerState) m_flowerState->ticksWithoutNectarSinceExitingHive = ticks;
    }

    int Bee::GetCropsGrownSincePollination() const {
        return m_flowerState ? m_flowerState->numCropsGrownSincePollination : 0;
    }

    void Bee::SetCropsGrownSincePollination(int n) {
        if (m_flowerState) m_flowerState->numCropsGrownSincePollination = n;
    }

    void Bee::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void Bee::Tick() {
        // MC Bee.tick: super, then updateRollAmount on BOTH sides (the nectar
        // drip particles need the particle system).
        GenericAnimal::Tick();
        UpdateRollAmount();
    }

    bool Bee::DoHurtTarget(Entity& target) {
        // MC Bee.doHurtTarget. The base call covers hurtServer + knockback
        // (MC hurts directly with the sting damage source; same numbers —
        // ATTACK_DAMAGE, zero attack knockback). setStingerCount on the
        // target is render-side stinger decals — nothing consumes it here.
        const bool wasHurt = GenericAnimal::DoHurtTarget(target);
        if (wasHurt) {
            if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
                // MC: POISON_SECONDS_NORMAL 10 / POISON_SECONDS_HARD 18,
                // nothing on EASY.
                int poisonSeconds = 0;
                if (m_level) {
                    if (m_level->GetDifficulty() == Difficulty::Normal) poisonSeconds = 10;
                    else if (m_level->GetDifficulty() == Difficulty::Hard) poisonSeconds = 18;
                }
                if (poisonSeconds > 0) {
                    living->AddEffect(
                        MobEffectInstance(MobEffectId::Poison, poisonSeconds * 20, 0),
                        this);
                }
            }
            // MC: setHasStung(true) + stopBeingAngry() (the BEE_STING sound
            // waits on the sound system).
            m_hasStung = true;
            StopBeingAngry();
        }
        return wasHurt;
    }

    void Bee::AiStep() {
        // MC Bee.aiStep's server half, minus the hive/flower cooldowns that
        // belong to the skipped goals. shouldRoll is MC's exact condition
        // now that anger exists.
        GenericAnimal::AiStep();
        if (m_level && !m_level->IsClientSide()) {
            LivingEntity* target = GetTarget();
            const bool shouldRoll = IsAngry() && !m_hasStung &&
                target != nullptr && target->DistanceToSqr(*this) < 4.0;
            m_rolling = shouldRoll;

            // MC Bee.aiStep: the new-flower search cooldown ticks down here.
            if (m_flowerState &&
                m_flowerState->remainingCooldownBeforeLocatingNewFlower > 0) {
                --m_flowerState->remainingCooldownBeforeLocatingNewFlower;
            }
        }
    }

    void Bee::CustomServerAiStep() {
        GenericAnimal::CustomServerAiStep();

        // MC Bee.customServerAiStep: the ticks-without-nectar clock that
        // BeeWanderGoal reads.
        if (m_flowerState && !m_flowerState->hasNectar) {
            ++m_flowerState->ticksWithoutNectarSinceExitingHive;
        }

        // The bee's own drown rule: 20 ticks fully in water, then 1.0 drown
        // damage every tick — much faster than the generic air supply (which
        // also runs, harmlessly behind this).
        if (IsInWater()) {
            ++m_underWaterTicks;
        } else {
            m_underWaterTicks = 0;
        }
        if (m_underWaterTicks > 20) {
            Hurt(MobDamageSource::Drown, 1.0f, nullptr);
        }

        // A stung bee dies within STING_DEATH_COUNTDOWN (1200) ticks — every
        // 5th tick rolls nextInt(clamp(1200 - t, 1, 1200)) == 0, so the odds
        // rise as the countdown shrinks and death is certain by the end.
        if (m_hasStung) {
            ++m_timeSinceSting;
            if (m_timeSinceSting % 5 == 0 &&
                m_level->Random().NextInt(
                    std::clamp(1200 - m_timeSinceSting, 1, 1200)) == 0) {
                Hurt(MobDamageSource::Generic, GetHealth(), nullptr);
            }
        }

        // MC: false — a bee's anger runs out even while it still has a
        // target; the grudge is the timer, nothing else.
        UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/false);
    }

    void Bee::UpdateRollAmount() {
        // MC Bee.updateRollAmount, constants verbatim.
        m_rollAmountO = m_rollAmount;
        if (IsRolling()) {
            m_rollAmount = std::min(1.0f, m_rollAmount + 0.2f);
        } else {
            m_rollAmount = std::max(0.0f, m_rollAmount - 0.24f);
        }
    }

    float Bee::GetRollAmount(float partialTick) const {
        // MC Bee.getRollAmount — the plain lerp.
        return m_rollAmountO + partialTick * (m_rollAmount - m_rollAmountO);
    }

    // ══ Breeze ═════════════════════════════════════════════════════════════

    Breeze::Breeze(EntityLevel* level) : GenericMonster(EntityTypeId::Breeze, level) {
        // NO GOALS — MC's Breeze is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Breeze's constructor maluses.
        SetPathfindingMalus(PathType::DangerTrapdoor, -1.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        BreezeAi::InitBrain(*this, *m_brain);
    }

    void Breeze::UpdateBrainActivity() { BreezeAi::UpdateActivity(*this); }

    bool Breeze::CanAttack(const LivingEntity& target) const {
        // MC Breeze.canAttack — players and iron golems, nothing else. The
        // base adds MC's alive/attackable gate, which its callers apply.
        return (target.IsPlayer() || target.GetType() == EntityTypeId::IronGolem)
            && Mob::CanAttack(target);
    }

    bool Breeze::WithinInnerCircleRange(const glm::dvec3& target) const {
        // MC target.closerThan(blockPosition().getCenter(), 4.0, 10.0) — XZ
        // and Y tested separately.
        const glm::ivec3 bp = BlockPosition();
        const double dx = target.x - (bp.x + 0.5);
        const double dy = target.y - (bp.y + 0.5);
        const double dz = target.z - (bp.z + 0.5);
        return dx * dx + dz * dz < 4.0 * 4.0 && std::abs(dy) < 10.0;
    }

    LivingEntity* Breeze::GetHurtBy() const {
        // MC Breeze.getHurtBy reads HURT_BY's DamageSource entity; this port's
        // HurtBySensor parks the attacker in HURT_BY_ENTITY instead.
        const Brain* brain = GetBrain();
        return brain ? dynamic_cast<LivingEntity*>(
                           brain->GetEntity(MemoryModule::HurtByEntity))
                     : nullptr;
    }

    void Breeze::Tick() {
        // MC Breeze.tick runs this before super.tick(). The per-pose ground
        // and jump-trail particles have no particle system to land in; the
        // animation half is complete. Timers are client state, so the block is
        // client-gated — MC runs it on both sides but only the client reads
        // the states.
        if (m_level && m_level->IsClientSide()) {
            const Pose pose = GetPose();
            if (pose == Pose::LongJumping) {
                Anim(MobAnim::LongJump).StartIfStopped(tickCount);
            }
            Anim(MobAnim::Idle).StartIfStopped(tickCount);
            // MC: leaving SLIDING plays slideBack from the top — the little
            // recover shuffle after every slide.
            if (pose != Pose::Sliding && Anim(MobAnim::Slide).IsStarted()) {
                Anim(MobAnim::SlideBack).Start(tickCount);
                Anim(MobAnim::Slide).Stop();
            }
        }
        // MC's 1–80-tick whirl-sound timer would run here; no sound system.
        GenericMonster::Tick();
    }

    void Breeze::ResetAnimations() {
        // MC Breeze.resetAnimations. Slide is deliberately NOT here — the
        // slide→slideBack transition in Tick has to see it still running.
        Anim(MobAnim::Shoot).Stop();
        Anim(MobAnim::Idle).Stop();
        Anim(MobAnim::Inhale).Stop();
        Anim(MobAnim::LongJump).Stop();
    }

    void Breeze::OnPoseUpdated() {
        // MC Breeze.onSyncedDataUpdated's DATA_POSE branch — client only.
        if (!m_level || !m_level->IsClientSide()) return;
        ResetAnimations();
        switch (GetPose()) {
            case Pose::Shooting: Anim(MobAnim::Shoot).StartIfStopped(tickCount); break;
            case Pose::Inhaling: Anim(MobAnim::Inhale).StartIfStopped(tickCount); break;
            case Pose::Sliding:  Anim(MobAnim::Slide).StartIfStopped(tickCount); break;
            default: break;
        }
    }

    void Breeze::ShootWindCharge(double xd, double yd, double zd, float inaccuracy) {
        if (!m_level || m_level->IsClientSide()) return;
        // MC Shoot.tick: BreezeWindCharge(breeze, level) — spawned at
        // (x, getFiringYPosition(), z) — then
        // Projectile.spawnProjectileUsingShoot(..., 0.7F, inaccuracy), which
        // is exactly Projectile::Shoot's normalize + triangle jitter + scale.
        auto charge = std::make_unique<BreezeWindCharge>(m_level);
        charge->SetOwner(this);
        charge->position = glm::dvec3(position.x, GetFiringYPosition(), position.z);
        charge->Shoot(xd, yd, zd, 0.7f, inaccuracy);
        m_level->AddFreshEntity(std::move(charge));
    }

    // ══ Warden ═════════════════════════════════════════════════════════════

    namespace {
        // MC's shared "this mob just landed a melee hit" entity event. Warden
        // and creaking both use it to start their attack animation on every
        // client that can see them.
        constexpr uint8_t kEventMobAttack = 4;
        // MC Warden's own events: 61 pulses the tendrils (a model hook the
        // generated warden model does not have), 62 starts the sonic boom.
        constexpr uint8_t kEventWardenTendrils = 61;
        constexpr uint8_t kEventWardenSonicBoom = 62;
        // MC Creaking's invulnerability shimmer.
        constexpr uint8_t kEventCreakingInvulnerable = 66;
    }

    Warden::Warden(EntityLevel* level) : GenericMonster(EntityTypeId::Warden, level) {
        // NO GOALS — MC's Warden is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Warden's constructor maluses — it wades lava and walks fire.
        SetPathfindingMalus(PathType::UnpassableRail, 0.0f);
        SetPathfindingMalus(PathType::DamageOther, 8.0f);
        SetPathfindingMalus(PathType::PowderSnow, 8.0f);
        SetPathfindingMalus(PathType::Lava, 8.0f);
        SetPathfindingMalus(PathType::DamageFire, 0.0f);
        SetPathfindingMalus(PathType::DangerFire, 0.0f);

        m_brain = std::make_unique<Brain>();
        WardenAi::InitBrain(*this, *m_brain);
    }

    bool Warden::IsDiggingOrEmerging() const {
        return GetPose() == Pose::Digging || GetPose() == Pose::Emerging;
    }

    bool Warden::CanTargetEntity(const Entity* entity) const {
        // MC Warden.canTargetEntity: living, not another warden, not
        // creative/spectator, vulnerable and alive. (Armor stands and the
        // world border have no equivalents.)
        auto* living = dynamic_cast<const LivingEntity*>(entity);
        if (!living || living == this) return false;
        if (living->GetType() == EntityTypeId::Warden) return false;
        if (living->IsCreative() || living->IsSpectator()) return false;
        return living->IsAttackable() && living->IsAlive();
    }

    std::shared_ptr<SpawnGroupData>
    Warden::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (Brain* brain = GetBrain()) {
            brain->SetMemoryWithExpiry(MemoryModule::DigCooldown, std::monostate{},
                                       WardenAi::kDiggingCooldown);
        }
        // MC: only EntitySpawnReason.TRIGGERED — a sculk shrieker's summon —
        // spawns the warden EMERGING (pose + IS_EMERGING for the emerge
        // activity). No shriekers and no TRIGGERED reason exist yet, so every
        // warden here starts above ground, exactly like MC's /summon; the
        // whole emerge path below is wired for when they do.
        (void)reason;
        return GenericMonster::FinalizeSpawn(reason, std::move(groupData));
    }

    void Warden::Tick() {
        // MC Warden.tick, server half: a persistent warden never digs away.
        // (The VibrationSystem ticker that precedes this in MC has no
        // game-event engine to tick — see the class comment.)
        if (m_level && !m_level->IsClientSide()
            && (IsPersistenceRequired() || RequiresCustomPersistence())) {
            WardenAi::SetDigCooldown(*this);
        }
        GenericMonster::Tick();
        // MC's client half — heartbeat sound, tendril/heart counters, digging
        // particles — is model-layer work the generated warden has no hooks
        // for.
    }

    void Warden::UpdateBrainActivity() {
        // MC Warden.customServerAiStep's order around the brain tick: anger
        // decays every 20 ticks, THEN the activity switch reads the result.
        // (applyDarknessAround is skipped even with the effect system in:
        // DARKNESS is a pure screen effect — it has no server-side gameplay
        // half — and no effect sync/rendering exists to show it.)
        if (tickCount % 20 == 0) TickAngerManagement();
        WardenAi::UpdateActivity(*this);
    }

    void Warden::TickAngerManagement() {
        // MC AngerManagement.tick: every suspect loses 1 anger per second and
        // drops off at ≤1, on death, or on becoming untargetable. The
        // UUID-persistence half is gone with mob saving.
        for (auto it = m_anger.begin(); it != m_anger.end();) {
            if (it->anger > 1 && !it->entity->IsRemoved()
                && CanTargetEntity(it->entity)) {
                --(it->anger);
                ++it;
            } else {
                it = m_anger.erase(it);
            }
        }
        SortAnger();
    }

    void Warden::SortAnger() {
        // MC AngerManagement.Sorter: angry suspects first, then players, then
        // by anger — which is why a warden mid-rampage swings to whichever
        // PLAYER made it angry rather than the zombie that shoved it.
        std::stable_sort(m_anger.begin(), m_anger.end(),
                         [](const AngerEntry& a, const AngerEntry& b) {
                             const bool angryA = a.anger >= kAngerAngry;
                             const bool angryB = b.anger >= kAngerAngry;
                             if (angryA != angryB) return angryA;
                             const bool playerA = a.entity->IsPlayer();
                             const bool playerB = b.entity->IsPlayer();
                             if (playerA != playerB) return playerA;
                             return a.anger > b.anger;
                         });
    }

    void Warden::IncreaseAngerAt(Entity* entity, int amount, bool playSound) {
        // MC Warden.increaseAngerAt. playSound picks WARDEN_LISTENING[-ANGRY];
        // no sound system.
        (void)playSound;
        if (IsNoAi() || !CanTargetEntity(entity)) return;
        WardenAi::SetDigCooldown(*this);

        Brain* brain = GetBrain();
        Entity* currentTarget = brain ? brain->GetEntity(MemoryModule::AttackTarget)
                                      : nullptr;
        const bool maybeSwitchTarget = !(currentTarget && currentTarget->IsPlayer());

        // MC AngerManagement.increaseAnger — clamp at 150.
        int newAnger = 0;
        bool found = false;
        for (AngerEntry& e : m_anger) {
            if (e.entity == entity) {
                e.anger = std::min(150, e.anger + amount);
                newAnger = e.anger;
                found = true;
                break;
            }
        }
        if (!found) {
            newAnger = std::min(150, amount);
            m_anger.push_back({ entity, newAnger });
        }
        SortAnger();

        // MC: a player crossing the ANGRY line evicts a non-player target so
        // the roar/fight re-acquires against the player.
        if (brain && entity->IsPlayer() && maybeSwitchTarget
            && newAnger >= kAngerAngry) {
            brain->EraseMemory(MemoryModule::AttackTarget);
        }
    }

    void Warden::ClearAnger(const Entity* entity) {
        for (auto it = m_anger.begin(); it != m_anger.end(); ++it) {
            if (it->entity == entity) {
                m_anger.erase(it);
                break;
            }
        }
        SortAnger();
    }

    int Warden::GetActiveAnger() const {
        // MC AngerManagement.getActiveAnger(getTarget()): with a target its
        // anger, without one the highest on the books.
        const Brain* brain = GetBrain();
        const Entity* target = brain ? brain->GetEntity(MemoryModule::AttackTarget)
                                     : nullptr;
        if (target) {
            for (const AngerEntry& e : m_anger) {
                if (e.entity == target) return e.anger;
            }
            return 0;
        }
        int highest = 0;
        for (const AngerEntry& e : m_anger) highest = std::max(highest, e.anger);
        return highest;
    }

    LivingEntity* Warden::GetEntityAngryAt() const {
        // MC Warden.getEntityAngryAt — only an ANGRY warden names a culprit,
        // and it is the sort's top targetable suspect.
        if (!IsAngry()) return nullptr;
        for (const AngerEntry& e : m_anger) {
            if (CanTargetEntity(e.entity)) {
                return dynamic_cast<LivingEntity*>(e.entity);
            }
        }
        return nullptr;
    }

    void Warden::SetAttackTarget(LivingEntity* target) {
        // MC Warden.setAttackTarget — also arms the 200-tick melee-first
        // window before the first sonic boom (TIME_TO_USE_MELEE_UNTIL_SONIC_BOOM).
        if (Brain* brain = GetBrain()) {
            brain->EraseMemory(MemoryModule::RoarTarget);
            brain->SetMemory(MemoryModule::AttackTarget, static_cast<Entity*>(target));
            brain->EraseMemory(MemoryModule::CantReachWalkTargetSince);
            brain->SetMemoryWithExpiry(MemoryModule::SonicBoomCooldown,
                                       std::monostate{}, 200);
        }
    }

    bool Warden::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Warden.isInvulnerableTo: unhittable while digging or emerging,
        // except by BYPASSES_INVULNERABILITY damage (the void).
        if (IsDiggingOrEmerging() && source != MobDamageSource::Void) return false;

        const bool hurt = GenericMonster::Hurt(source, amount, attacker);
        // MC anger-boosts on every hurtServer call, NOT only when the damage
        // landed — a hit swallowed by the invulnerability window still angers.
        if (m_level && !m_level->IsClientSide() && !IsNoAi()) {
            // MC Warden.hurtServer: being hit is instant maximum anger.
            IncreaseAngerAt(attacker, kAngerAngry + 20, false);
            Brain* brain = GetBrain();
            auto* livingAttacker = dynamic_cast<LivingEntity*>(attacker);
            if (brain && !brain->HasMemoryValue(MemoryModule::AttackTarget)
                && livingAttacker) {
                // MC: a direct hit, or any hit from inside 5 blocks, is
                // answered immediately, anger ladder or no.
                const bool direct = source != MobDamageSource::Projectile;
                if (direct || DistanceToSqr(*livingAttacker) < 5.0 * 5.0) {
                    SetAttackTarget(livingAttacker);
                }
            }
        }
        return hurt;
    }

    bool Warden::DoHurtTarget(Entity& target) {
        // MC Warden.doHurtTarget broadcasts BEFORE delegating, so the animation
        // starts on the same tick the damage lands rather than the next one —
        // and every landed swing re-arms the 40-tick sonic-boom cooldown.
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
        if (Brain* brain = GetBrain()) {
            brain->SetMemoryWithExpiry(MemoryModule::SonicBoomCooldown,
                                       std::monostate{}, 40);
        }
        return GenericMonster::DoHurtTarget(target);
    }

    void Warden::HandleEntityEvent(uint8_t id) {
        if (id == kEventMobAttack) {
            // MC stops the roar first: the two clips write the same parts, and
            // a roar left running would fight the swing.
            Anim(MobAnim::Roar).Stop();
            Anim(MobAnim::Attack).Start(tickCount);
            return;
        }
        if (id == kEventWardenTendrils) {
            // MC pulses tendrilAnimation for 10 ticks; the generated model has
            // no tendril channel to read it. Swallowed so the base does not
            // mistake it for something else.
            return;
        }
        if (id == kEventWardenSonicBoom) {
            Anim(MobAnim::SonicBoom).Start(tickCount);
            return;
        }
        GenericMonster::HandleEntityEvent(id);
    }

    void Warden::OnPoseUpdated() {
        // MC Warden.onSyncedDataUpdated's DATA_POSE branch. `start`, not
        // `startIfStopped`: each of these fires exactly on the transition and
        // plays once from the top. Client only — the server sets these poses.
        if (!m_level || !m_level->IsClientSide()) return;
        switch (GetPose()) {
            case Pose::Emerging: Anim(MobAnim::Emerge).Start(tickCount); break;
            case Pose::Digging:  Anim(MobAnim::Digging).Start(tickCount); break;
            case Pose::Roaring:  Anim(MobAnim::Roar).Start(tickCount); break;
            case Pose::Sniffing: Anim(MobAnim::Sniff).Start(tickCount); break;
            default: break;
        }
    }

    void Warden::ClearReferenceTo(const Entity* entity) {
        GenericMonster::ClearReferenceTo(entity);
        // The anger map holds raw pointers; a removed entity must not survive
        // in it — same reasoning as Goal::ClearReferenceTo.
        for (auto it = m_anger.begin(); it != m_anger.end(); ++it) {
            if (it->entity == entity) {
                m_anger.erase(it);
                break;
            }
        }
    }

    // ══ Creaking ═══════════════════════════════════════════════════════════

    namespace {

        // MC Creaking's gated controls: a frozen creaking's steering, gaze and
        // pathing all no-op, which is what makes the freeze absolute instead
        // of merely slow.
        class CreakingMoveControl : public MoveControl {
        public:
            explicit CreakingMoveControl(Creaking* creaking)
                : MoveControl(creaking), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) MoveControl::Tick();
            }
        private:
            Creaking* m_creaking;
        };

        class CreakingLookControl : public LookControl {
        public:
            explicit CreakingLookControl(Creaking* creaking)
                : LookControl(creaking), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) LookControl::Tick();
            }
        private:
            Creaking* m_creaking;
        };

        class CreakingPathNavigation : public GroundPathNavigation {
        public:
            CreakingPathNavigation(Creaking* creaking, EntityLevel* level)
                : GroundPathNavigation(creaking, level), m_creaking(creaking) {}
            void Tick() override {
                if (m_creaking->CanMove()) GroundPathNavigation::Tick();
            }
        private:
            Creaking* m_creaking;
        };

    } // namespace

    Creaking::Creaking(EntityLevel* level) : GenericMonster(EntityTypeId::Creaking, level) {
        // NO GOALS — MC's Creaking is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        SetMoveControl(std::make_unique<CreakingMoveControl>(this));
        SetLookControl(std::make_unique<CreakingLookControl>(this));
        SetNavigation(std::make_unique<CreakingPathNavigation>(this, level));
        // MC also gates JumpControl (its tick clears `jumping`). JumpControl's
        // Tick is virtual now (the rabbit needed it), but no override is
        // registered here: the only jump writer in this brain is Swim, which
        // CreakingAi already gates on canMove, so nothing can latch a jump
        // while frozen anyway.

        // MC's HomeNodeEvaluator (paths refuse to leave 32 blocks of the
        // creaking heart) needs a home position; heartless creakings have
        // none, in MC too.

        m_brain = std::make_unique<Brain>();
        CreakingAi::InitBrain(*this, *m_brain);
    }

    void Creaking::UpdateBrainActivity() { CreakingAi::UpdateActivity(*this); }

    bool Creaking::DoHurtTarget(Entity& target) {
        // MC refuses to swing at anything that is not alive, and the animation
        // is inside that guard — a creaking does not windmill at an item frame.
        if (!dynamic_cast<LivingEntity*>(&target)) return false;
        m_attackAnimationRemainingTicks = 15;
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
        return GenericMonster::DoHurtTarget(target);
    }

    bool Creaking::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Creaking.hurtServer's heart-bound branch, minus the heart: the
        // hits a heart would eat (a living attacker or a projectile) still
        // flash the 8-tick invulnerability shimmer, but the damage lands —
        // there is no heart to absorb it. See the class comment.
        if (m_level && !m_level->IsClientSide()
            && m_invulnerabilityAnimationRemainingTicks <= 0 && !IsDeadOrDying()
            && (dynamic_cast<LivingEntity*>(attacker)
                || source == MobDamageSource::Projectile)) {
            m_invulnerabilityAnimationRemainingTicks = 8;
            m_level->BroadcastEntityEvent(*this, kEventCreakingInvulnerable);
        }
        return GenericMonster::Hurt(source, amount, attacker);
    }

    void Creaking::HandleEntityEvent(uint8_t id) {
        if (id == kEventMobAttack) {
            m_attackAnimationRemainingTicks = 15;
            return;
        }
        if (id == kEventCreakingInvulnerable) {
            m_invulnerabilityAnimationRemainingTicks = 8;
            return;
        }
        GenericMonster::HandleEntityEvent(id);
    }

    void Creaking::AiStep() {
        // MC Creaking.aiStep: the two animation counters run on both sides
        // (the server sets them directly, the clients from entity events)...
        if (m_invulnerabilityAnimationRemainingTicks > 0) {
            --m_invulnerabilityAnimationRemainingTicks;
        }
        if (m_attackAnimationRemainingTicks > 0) {
            --m_attackAnimationRemainingTicks;
        }

        // ...and the freeze gate re-evaluates every tick, server-side.
        // Freezing plants the creaking mid-step; MC plays CREAKING_FREEZE /
        // UNFREEZE on the transition.
        if (m_level && !m_level->IsClientSide()) {
            const bool couldMove = m_canMove;
            const bool nowCanMove = CheckCanMove();
            if (nowCanMove != couldMove && !nowCanMove) StopInPlace();
            m_canMove = nowCanMove;
        }
        GenericMonster::AiStep();
    }

    void Creaking::Tick() {
        GenericMonster::Tick();
        // MC Creaking.tick also validates the creaking heart (none here) and
        // flickers the emissive eyes while dying (no emissive layer).
    }

    void Creaking::SetupAnimationStates() {
        // MC Creaking.setupAnimationStates, all three lines.
        Anim(MobAnim::Attack).AnimateWhen(m_attackAnimationRemainingTicks > 0, tickCount);
        Anim(MobAnim::Invulnerability)
            .AnimateWhen(m_invulnerabilityAnimationRemainingTicks > 0, tickCount);
        Anim(MobAnim::Death).AnimateWhen(IsTearingDown(), tickCount);
    }

    void Creaking::Die(MobDamageSource source, Entity* attacker) {
        // DEVIATION (see the class comment): MC tears down only a heart-bound
        // creaking; the twitch death is this port's death for every creaking.
        m_tearingDown = true;
        GenericMonster::Die(source, attacker);
    }

    void Creaking::TickDeath() {
        // MC Creaking.tickDeath while tearing down: 45 ticks
        // (TWITCH_DEATH_DURATION) instead of the 20-tick fall-over, then the
        // crumble — poof particles stand in for BLOCK_CRUMBLE pale oak.
        ++deathTime;
        if (deathTime > 45 && m_level && !m_level->IsClientSide() && !IsRemoved()) {
            m_level->BroadcastEntityEvent(*this, 60);
            Remove(RemovalReason::Killed);
        }
    }

    void Creaking::TickHeadTurn(float yBodyRotTarget) {
        // MC CreakingBodyRotationControl — the torso freezes with the rest.
        if (CanMove()) GenericMonster::TickHeadTurn(yBodyRotTarget);
    }

    void Creaking::Knockback(double power, double dx, double dz) {
        // MC Creaking.knockback — an unseen creaking cannot be shoved.
        if (CanMove()) GenericMonster::Knockback(power, dx, dz);
    }

    void Creaking::UpdateWalkAnimation(float distance) {
        // MC Creaking.updateWalkAnimation — ×25 with a 3.0 cap: the walk cycle
        // overdrives at speed, which is most of the creaking's scuttle.
        walkAnimation.Update(std::min(distance * 25.0f, 3.0f), 0.4f, 1.0f);
    }

    void Creaking::Activate(LivingEntity* player) {
        // MC Creaking.activate — CREAKING_ACTIVATE plays here when sounds
        // exist.
        if (Brain* brain = GetBrain()) {
            brain->SetMemory(MemoryModule::AttackTarget, static_cast<Entity*>(player));
        }
        m_isActive = true;
    }

    void Creaking::Deactivate() {
        if (Brain* brain = GetBrain()) {
            brain->EraseMemory(MemoryModule::AttackTarget);
        }
        m_isActive = false;
    }

    bool Creaking::CheckCanMove() {
        // MC Creaking.checkCanMove, verbatim minus the carved-pumpkin disguise
        // (no player equipment): an ACTIVE creaking freezes under any watching
        // eye; an inactive one activates on being watched inside 12 blocks
        // (ACTIVATION_RANGE_SQ = 144).
        const Brain* brain = GetBrain();
        const std::vector<Entity*>* players =
            brain ? brain->GetEntityList(MemoryModule::NearestPlayers) : nullptr;
        const bool active = IsActive();

        if (!players || players->empty()) {
            if (active) Deactivate();
            return true;
        }

        bool hasPotentialTarget = false;
        for (Entity* e : *players) {
            auto* player = dynamic_cast<LivingEntity*>(e);
            if (!player || !CanAttack(*player)) continue;
            hasPotentialTarget = true;
            if (IsLookingAtMe(*player)) {
                if (active) return false;
                if (player->DistanceToSqr(*this) < 144.0) {
                    Activate(player);
                    return false;
                }
            }
        }
        if (!hasPotentialTarget && active) Deactivate();
        return true;
    }

    bool Creaking::IsLookingAtMe(const LivingEntity& player) const {
        // MC isLookingAtMe(player, 0.5, scaleByDistance=false, visual=true,
        // eyeY, y + 0.5·scale, midpoint) — three heights so crouching behind a
        // half wall does not blind it.
        const glm::vec3 viewF = Mth::ViewVector(player.xRot, player.yRot);
        glm::dvec3 view(viewF.x, viewF.y, viewF.z);
        view = glm::normalize(view);

        const double candidates[3] = {
            GetEyeY(), position.y + 0.5, (GetEyeY() + position.y) / 2.0,
        };
        for (double y : candidates) {
            glm::dvec3 dir(position.x - player.position.x,
                           y - player.GetEyeY(),
                           position.z - player.position.z);
            const double len = glm::length(dir);
            if (len < 1.0e-8) continue;
            dir /= len;
            if (glm::dot(view, dir) > 1.0 - 0.5) {
                // The mutable line-of-sight cache is the same one every other
                // sight test uses; the const_cast is only ever this class
                // asking about itself.
                if (const_cast<Creaking*>(this)->GetSensing().HasLineOfSight(player)) {
                    return true;
                }
            }
        }
        return false;
    }

    // ══ Sniffer ════════════════════════════════════════════════════════════

    Sniffer::Sniffer(EntityLevel* level) : GenericAnimal(EntityTypeId::Sniffer, level) {
        // NO GOALS — MC's Sniffer is all brain.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC Sniffer's constructor maluses — it will not path into water at
        // all unless already burning or wet (that onPathfindingStart flip has
        // no hook here; the resting value is the behaviour that shows).
        SetPathfindingMalus(PathType::Water, -1.0f);
        SetPathfindingMalus(PathType::DangerPowderSnow, -1.0f);
        SetPathfindingMalus(PathType::DamageCautious, -1.0f);

        m_brain = std::make_unique<Brain>();
        SnifferAi::InitBrain(*this, *m_brain);
    }

    void Sniffer::UpdateBrainActivity() { SnifferAi::UpdateActivity(*this); }

    bool Sniffer::IsSnifferFood(uint32_t itemId) {
        // MC ItemTags.SNIFFER_FOOD — torchflower seeds only.
        static const ItemID seeds = RecipeManager::ItemFromSlug("torchflower_seeds");
        return seeds != Items::Air && itemId == static_cast<uint32_t>(seeds);
    }

    bool Sniffer::IsFood(uint32_t itemId) const { return IsSnifferFood(itemId); }

    Sniffer& Sniffer::TransitionTo(State state) {
        // MC Sniffer.transitionTo. Each branch's entry sound (SNIFFER_HAPPY,
        // SNIFFER_SNIFFING, SNIFFER_SCENTING, SNIFFER_DIGGING_STOP) waits on a
        // sound system.
        if (state == State::Digging) {
            // MC onDiggingStart: DATA_DROP_SEED_AT_TICK = now + 120 — the
            // seed pops out mid-dig, not at the end.
            m_dropSeedAtTick = tickCount + 120;
        }
        m_state = state;
        return *this;
    }

    void Sniffer::SetAnimStateByte(uint8_t v) {
        // MC Sniffer.onSyncedDataUpdated(DATA_STATE) — client side of the
        // state machine. Guarded on change: the tracker resends unchanged
        // bytes alongside every health tick.
        const State state = v <= 6 ? static_cast<State>(v) : State::Idling;
        if (state == m_state) return;
        m_state = state;
        ResetAnimations();
        switch (state) {
            case State::FeelingHappy:
                Anim(MobAnim::FeelingHappy).StartIfStopped(tickCount);
                break;
            case State::Scenting:
                Anim(MobAnim::Scenting).StartIfStopped(tickCount);
                break;
            case State::Sniffing:
                Anim(MobAnim::Sniffing).StartIfStopped(tickCount);
                break;
            case State::Digging:
                Anim(MobAnim::Digging).StartIfStopped(tickCount);
                break;
            case State::Rising:
                Anim(MobAnim::Rising).StartIfStopped(tickCount);
                break;
            default:
                // IDLING and SEARCHING carry no clip — the walk cycle is the
                // whole of their motion. MC also refreshDimensions()es here
                // (a digging sniffer is 0.4 shorter); no per-state dimensions
                // exist in this port.
                break;
        }
    }

    void Sniffer::ResetAnimations() {
        // MC Sniffer.resetAnimations.
        Anim(MobAnim::Digging).Stop();
        Anim(MobAnim::Sniffing).Stop();
        Anim(MobAnim::Rising).Stop();
        Anim(MobAnim::FeelingHappy).Stop();
        Anim(MobAnim::Scenting).Stop();
    }

    bool Sniffer::IsTempted() const {
        const Brain* brain = GetBrain();
        return brain && brain->GetBool(MemoryModule::IsTempted).value_or(false);
    }

    // Brain mobs panic through the IS_PANICKING memory, not the PanicGoal the
    // base class polls for.
    static bool IsBrainPanicking(const LivingEntity& body) {
        const Brain* brain = body.GetBrain();
        return brain && brain->HasMemoryValue(MemoryModule::IsPanicking);
    }

    bool Sniffer::CanSniff() const {
        // MC Sniffer.canSniff — leashes and riding do not exist.
        return !IsTempted() && !IsBrainPanicking(*this) && !IsInWater()
            && !IsInLove() && onGround;
    }

    bool Sniffer::CanDig() const {
        return !IsBrainPanicking(*this) && !IsTempted() && !IsBaby() && !IsInWater()
            && onGround && CanDigAt(GetHeadBlock() - glm::ivec3(0, 1, 0));
    }

    glm::ivec3 Sniffer::GetHeadBlock() const {
        // MC Sniffer.getHeadBlock — 2.25 blocks along the view, 0.2 up.
        const glm::vec3 forward = Mth::ViewVector(xRot, yRot);
        const glm::dvec3 head = position + glm::dvec3(forward) * 2.25;
        return glm::ivec3(static_cast<int>(std::floor(head.x)),
                          static_cast<int>(std::floor(position.y + 0.2)),
                          static_cast<int>(std::floor(head.z)));
    }

    bool Sniffer::CanDigAt(const glm::ivec3& pos) const {
        if (!m_level) return false;
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC BlockTags.SNIFFER_DIGGABLE_BLOCK, flattened — the eight dirts.
        switch (blocks->GetBlock(pos.x, pos.y, pos.z)) {
            case BlockID::Dirt:
            case BlockID::Grass:
            case BlockID::Podzol:
            case BlockID::CoarseDirt:
            case BlockID::RootedDirt:
            case BlockID::MossBlock:
            case BlockID::Mud:
            case BlockID::MuddyMangroveRoots:
                break;
            default:
                return false;
        }
        // MC: never re-dig an explored column...
        for (const glm::ivec3& explored : m_exploredPositions) {
            if (explored == pos) return false;
        }
        // ...and the spot must actually be reachable (createPath(pos, 1)
        // .canReach()).
        auto* self = const_cast<Sniffer*>(this);
        std::optional<Path> path = self->GetNavigation().CreatePath(pos, 1);
        return path && path->CanReach();
    }

    std::optional<glm::ivec3> Sniffer::CalculateDigPosition() {
        // MC Sniffer.calculateDigPosition — five LandRandomPos rolls of
        // widening radius (10, 12, 14, 16, 18), first whose BELOW block is
        // diggable wins.
        for (int i = 0; i < 5; ++i) {
            const std::optional<glm::dvec3> pos =
                RandomPos::GetLandPos(*this, 10 + 2 * i, 3);
            if (!pos) continue;
            const glm::ivec3 below(static_cast<int>(std::floor(pos->x)),
                                   static_cast<int>(std::floor(pos->y)) - 1,
                                   static_cast<int>(std::floor(pos->z)));
            if (CanDigAt(below)) return below;
        }
        return std::nullopt;
    }

    void Sniffer::OnDiggingComplete(bool success) {
        if (!success) return;
        // MC storeExploredPosition(getOnPos()) — newest first, 20 remembered.
        if (m_exploredPositions.size() > 20) m_exploredPositions.resize(20);
        m_exploredPositions.insert(m_exploredPositions.begin(),
                                   BlockPosition() - glm::ivec3(0, 1, 0));
    }

    void Sniffer::Tick() {
        // MC Sniffer.tick: SEARCHING loops its sound (client, absent) and
        // DIGGING emits particles (absent) and drops the seed.
        if (m_level && !m_level->IsClientSide() && m_state == State::Digging) {
            DropSeed();
        }
        GenericAnimal::Tick();
    }

    void Sniffer::DropSeed() {
        // MC Sniffer.dropSeed — one pull from the SNIFFER_DIGGING loot table
        // (torchflower seeds or a pitcher pod, equal weight) at the head
        // block, exactly at DATA_DROP_SEED_AT_TICK.
        if (!m_level || m_dropSeedAtTick != tickCount) return;
        static const ItemID seeds = RecipeManager::ItemFromSlug("torchflower_seeds");
        static const ItemID pod   = RecipeManager::ItemFromSlug("pitcher_pod");
        const ItemID pick = m_level->Random().NextBool() ? seeds : pod;
        if (pick == Items::Air) return;
        const glm::ivec3 head = GetHeadBlock();
        m_level->SpawnItemDrop(glm::dvec3(head.x + 0.5, head.y + 0.5, head.z + 0.5),
                               static_cast<uint32_t>(pick), 1);
    }

    void Sniffer::Die(MobDamageSource source, Entity* attacker) {
        // MC Sniffer.die resets to IDLING first, so the corpse is not stuck
        // nose-down in the dirt.
        TransitionTo(State::Idling);
        GenericAnimal::Die(source, attacker);
    }

    // ══ CopperGolem ════════════════════════════════════════════════════════

    CopperGolem::CopperGolem(EntityLevel* level)
        : GenericPathfinderMob(EntityTypeId::CopperGolem, level) {
        // NO GOALS — MC's CopperGolem is all brain (AbstractGolem registers
        // none of its own either).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        // MC CopperGolem's constructor, line for line. (The IGNORE/UNSET
        // weathering tick setup is skipped — no weathering system.)
        GetNavigation().SetRequiredPathLength(48.0f);
        GetNavigation().SetCanOpenDoors(true);
        SetPersistenceRequired(true);
        SetState(State::Idle);
        SetPathfindingMalus(PathType::DangerFire, 16.0f);
        SetPathfindingMalus(PathType::DangerOther, 16.0f);
        SetPathfindingMalus(PathType::DamageFire, -1.0f);

        m_brain = std::make_unique<Brain>();
        CopperGolemAi::InitBrain(*this, *m_brain);
        // MC: getBrain().setMemory(TRANSPORT_ITEMS_COOLDOWN_TICKS,
        // random.nextInt(60, 100)) — the first trip waits out the spawn
        // cooldown.
        if (m_level) {
            m_brain->SetMemory(MemoryModule::TransportItemsCooldownTicks,
                               m_level->Random().NextInt(kSpawnCooldownMin,
                                                         kSpawnCooldownMax));
        }
    }

    void CopperGolem::UpdateBrainActivity() { CopperGolemAi::UpdateActivity(*this); }

    bool CopperGolem::IsHoldingItem() const {
        return m_level && m_level->IsClientSide() ? m_clientHoldingItem
                                                  : !m_handItem.IsEmpty();
    }

    uint8_t CopperGolem::GetAnimStateByte() const {
        return static_cast<uint8_t>((static_cast<uint8_t>(m_state) & 0x7)
                                    | (IsHoldingItem() ? 0x8 : 0));
    }

    void CopperGolem::SetAnimStateByte(uint8_t v) {
        // Client side of MC's COPPER_GOLEM_STATE synched data. No clip starts
        // here: SetupAnimationStates below runs every client tick and derives
        // them from the state, exactly as MC's does.
        const uint8_t id = v & 0x7;
        m_state = id <= 4 ? static_cast<State>(id) : State::Idle;
        m_clientHoldingItem = (v & 0x8) != 0;
    }

    void CopperGolem::SetupAnimationStates() {
        // MC CopperGolem.setupAnimationStates, case for case.
        switch (m_state) {
            case State::Idle:
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                if (m_idleAnimationStartTick == tickCount) {
                    Anim(MobAnim::Idle).Start(tickCount);
                } else if (m_idleAnimationStartTick == 0) {
                    m_idleAnimationStartTick =
                        tickCount + m_level->Random().NextInt(kSpinAnimationMinCooldown,
                                                              kSpinAnimationMaxCooldown);
                }
                // MC: SPIN_SOUND_TIME_INTERVAL_OFFSET ticks into the spin.
                // The playHeadSpinSound half waits on a sound system; the
                // re-arm is what keeps the spin periodic.
                if (tickCount == m_idleAnimationStartTick + 10) {
                    m_idleAnimationStartTick = 0;
                }
                break;
            case State::GettingItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionGetItem).StartIfStopped(tickCount);
                break;
            case State::GettingNoItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).StartIfStopped(tickCount);
                break;
            case State::DroppingItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).StartIfStopped(tickCount);
                break;
            case State::DroppingNoItem:
                Anim(MobAnim::Idle).Stop();
                m_idleAnimationStartTick = 0;
                Anim(MobAnim::InteractionGetItem).Stop();
                Anim(MobAnim::InteractionGetNoItem).Stop();
                Anim(MobAnim::InteractionDropItem).Stop();
                Anim(MobAnim::InteractionDropNoItem).StartIfStopped(tickCount);
                break;
        }
    }

    UseResult CopperGolem::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC CopperGolem.mobInteract's first branch: an empty player hand
        // takes what the golem carries. The remaining branches are skipped,
        // each waiting on its system: shears → the antenna equipment slot,
        // honeycomb waxing and the two axe scrapes → the weathering ladder.
        if (held.IsEmpty() && !GetMainHandItem().IsEmpty()) {
            if (m_level && m_level->IsClientSide()) return UseResult::Success;
            // MC BehaviorUtils.throwItem(this, equippedItem, player.position())
            // — no thrown-toward-a-point drop exists, so it pops at the golem.
            const ItemStack equipped = GetMainHandItem();
            if (m_level) {
                m_level->SpawnItemDrop(position + glm::dvec3(0.0, 0.5, 0.0),
                                       static_cast<uint32_t>(equipped.itemId),
                                       equipped.count);
            }
            SetItemInHand(ItemStack{});
            (void)player;
            return UseResult::Success;
        }
        return GenericPathfinderMob::MobInteract(player, held);
    }

    void CopperGolem::ActuallyHurt(MobDamageSource source, float amount,
                                   Entity* attacker) {
        // MC CopperGolem.actuallyHurt — super first, then the pose reset.
        GenericPathfinderMob::ActuallyHurt(source, amount, attacker);
        SetState(State::Idle);
    }

    void CopperGolem::Die(MobDamageSource source, Entity* attacker) {
        // MC dropEquipment → dropPreservedEquipment: the carried stack is a
        // guaranteed drop (pickUpItems calls setGuaranteedDrop). The antenna
        // slot's drop is skipped — no equipment system.
        if (m_level && !m_level->IsClientSide() && !m_handItem.IsEmpty()) {
            m_level->SpawnItemDrop(position + glm::dvec3(0.0, 0.5, 0.0),
                                   static_cast<uint32_t>(m_handItem.itemId),
                                   m_handItem.count);
            m_handItem.Clear();
        }
        GenericPathfinderMob::Die(source, attacker);
    }

    // ══ Armadillo ══════════════════════════════════════════════════════════

    namespace {

        // MC EntityTypeTags.UNDEAD, flattened from
        // data/minecraft/tags/entity_type/{undead,skeletons,zombies}.json.
        // A tag lookup would be the general answer, but entity-type tags are
        // not loaded anywhere else in this port and this is their only reader.
        bool IsUndeadType(EntityTypeId t) {
            switch (t) {
                case EntityTypeId::Skeleton:
                case EntityTypeId::Stray:
                case EntityTypeId::WitherSkeleton:
                case EntityTypeId::SkeletonHorse:
                case EntityTypeId::Bogged:
                case EntityTypeId::Parched:
                case EntityTypeId::ZombieHorse:
                case EntityTypeId::CamelHusk:
                case EntityTypeId::Zombie:
                case EntityTypeId::ZombieVillager:
                case EntityTypeId::ZombifiedPiglin:
                case EntityTypeId::Zoglin:
                case EntityTypeId::Drowned:
                case EntityTypeId::Husk:
                case EntityTypeId::ZombieNautilus:
                case EntityTypeId::Wither:
                case EntityTypeId::Phantom:
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    int Armadillo::AnimationDuration(State s) {
        // MC Armadillo.ArmadilloState's third constructor argument.
        switch (s) {
            case State::Idle:      return 0;
            case State::Rolling:   return 10;
            case State::Scared:    return 50;
            case State::Unrolling: return 30;
        }
        return 0;
    }

    bool Armadillo::ShouldHideInShell(State s, int64_t ticksInState) {
        // MC's per-constant override. The asymmetry is the point: rolling up
        // hides the body a little AFTER the roll starts (5 of 10 ticks), while
        // unrolling shows it a little BEFORE the unroll finishes (26 of 30), so
        // the swap never happens on a visible frame.
        switch (s) {
            case State::Idle:      return false;
            case State::Rolling:   return ticksInState > 5;
            case State::Scared:    return true;
            case State::Unrolling: return ticksInState < 26;
        }
        return false;
    }

    Armadillo::Armadillo(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Armadillo, level) {
        // NO GOALS — MC's Armadillo never registers any; the whole roll-up
        // life (ArmadilloBallUp, the scare sensor, the peek timers) is
        // ArmadilloAi's brain now, where MC keeps it.
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        ArmadilloAi::InitBrain(*this, *m_brain);
    }

    void Armadillo::UpdateBrainActivity() { ArmadilloAi::UpdateActivity(*this); }

    void Armadillo::SwitchToState(State s) {
        if (m_state == s) return;
        m_state = s;
        // MC resets inStateTicks in onSyncedDataUpdated, i.e. on BOTH sides,
        // which is what keeps the client's shouldHideInShell in step with the
        // animation it is playing.
        m_inStateTicks = 0;
    }

    int64_t Armadillo::DangerTicksRemaining() const {
        // MC brain.getTimeUntilExpiry(DANGER_DETECTED_RECENTLY).
        const Brain* brain = GetBrain();
        return brain ? brain->GetTimeUntilExpiry(MemoryModule::DangerDetectedRecently)
                     : 0;
    }

    bool Armadillo::IsScaredBy(const LivingEntity& other) const {
        // MC inflates the ARMADILLO's box, not the other entity's.
        AABB box = GetAABB();
        box.min -= glm::vec3(7.0f, 2.0f, 7.0f);
        box.max += glm::vec3(7.0f, 2.0f, 7.0f);
        if (!box.Intersects(other.GetAABB())) return false;

        // MC's three cases: anything in EntityTypeTags.UNDEAD, whatever last
        // hurt it, and a player who is sprinting or riding. A walking player
        // deliberately does NOT scare it — that is how you get close enough to
        // brush a scute off.
        if (IsUndeadType(other.GetType())) return true;
        // An identity compare, not a resolve: "did this thing hurt me" is a
        // question about who `other` IS, and answering it must not depend on
        // the attacker currently being loaded. Also keeps this method const.
        if (LastHurtByMobRef().Matches(other)) return true;
        if (other.IsPlayer()) {
            // MC Armadillo.java:227-228 — a spectator never scares it, checked
            // BEFORE the sprint test. This mattered even before the spectator
            // filter landed in GetEntitiesInBox, because isScaredBy is reached
            // from a sensor that uses MC's THREE-arg getEntitiesOfClass, which
            // carries no NO_SPECTATORS predicate.
            if (other.IsSpectator()) return false;
            // MC also counts a player who is a passenger; nothing here rides
            // anything, so sprinting is the whole of the reachable condition.
            return other.IsSprinting();
        }
        return false;
    }

    void Armadillo::RollUp() {
        if (IsScared()) return;
        StopInPlace();
        ResetLove();
        SwitchToState(State::Rolling);
    }

    void Armadillo::RollOut() {
        if (!IsScared()) return;
        SwitchToState(State::Idle);
    }

    void Armadillo::Tick() {
        GenericAnimal::Tick();

        // MC clamps the head to the body while scared, so a rolled-up
        // armadillo does not track you from inside its shell.
        if (IsScared()) {
            yHeadRot = yBodyRot;
        }

        ++m_inStateTicks;
    }

    void Armadillo::SetupAnimationStates() {
        // MC Armadillo.setupAnimationStates.
        switch (m_state) {
            case State::Idle:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).Stop();
                Anim(MobAnim::Peek).Stop();
                break;
            case State::Rolling:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).StartIfStopped(tickCount);
                Anim(MobAnim::Peek).Stop();
                break;
            case State::Scared:
                Anim(MobAnim::RollOut).Stop();
                Anim(MobAnim::RollUp).Stop();
                if (m_peekReceivedClient) {
                    Anim(MobAnim::Peek).Stop();
                    m_peekReceivedClient = false;
                }
                if (m_inStateTicks == 0) {
                    // Entering SCARED from a roll-up: the peek clip is started
                    // and immediately fast-forwarded past its whole duration,
                    // so the armadillo holds the closed pose instead of
                    // replaying the peek every time it re-hides.
                    Anim(MobAnim::Peek).Start(tickCount);
                    Anim(MobAnim::Peek).FastForward(
                        AnimationDuration(State::Scared), 1.0f);
                } else {
                    Anim(MobAnim::Peek).StartIfStopped(tickCount);
                }
                break;
            case State::Unrolling:
                Anim(MobAnim::RollOut).StartIfStopped(tickCount);
                Anim(MobAnim::RollUp).Stop();
                Anim(MobAnim::Peek).Stop();
                break;
        }
    }

    void Armadillo::HandleEntityEvent(uint8_t id) {
        // MC Armadillo.handleEntityEvent: 64 is "peek now".
        if (id == 64) {
            m_peekReceivedClient = true;
            return;
        }
        GenericAnimal::HandleEntityEvent(id);
    }

    bool Armadillo::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC Armadillo.hurtServer — the shell is worth about half the damage.
        if (IsScared()) amount = (amount - 1.0f) / 2.0f;

        const bool hurt = GenericAnimal::Hurt(source, amount, attacker);

        // MC Armadillo.actuallyHurt: being hit by something alive re-arms the
        // danger memory for 80 ticks and curls it up on the spot.
        if (hurt && !IsNoAi() && IsAlive() && dynamic_cast<LivingEntity*>(attacker)) {
            if (Brain* brain = GetBrain()) {
                brain->SetMemoryWithExpiry(MemoryModule::DangerDetectedRecently,
                                           true, 80);
            }
            if (CanStayRolledUp()) RollUp();
        }
        return hurt;
    }

    // ══ Allay ══════════════════════════════════════════════════════════════

    Allay::Allay(EntityLevel* level)
        : GenericPathfinderMob(EntityTypeId::Allay, level) {
        // NO GOALS — MC's Allay never registers any; its whole behaviour is
        // the brain (the def's flying navigation + hover move control stand).
        m_goalSelector.Clear();
        m_targetSelector.Clear();

        m_brain = std::make_unique<Brain>();
        AllayAi::InitBrain(*this, *m_brain);
    }

    void Allay::UpdateBrainActivity() { AllayAi::UpdateActivity(*this); }

    // ══ HappyGhast ═════════════════════════════════════════════════════════

    HappyGhast::HappyGhast(EntityLevel* level)
        : GenericAnimal(EntityTypeId::HappyGhast, level) {
        // A fresh happy ghast is an adult until FinalizeSpawn/CreateBaby says
        // otherwise; the age poll in CustomServerAiStep swaps setups at the
        // boundary — the port's AgeableMob has no ageBoundaryReached hook, so
        // a ghastling runs its first tick on the adult set (invisible: one
        // tick, no goal completes).
        RegisterAdultGoals();
    }

    void HappyGhast::RegisterAdultGoals() {
        // GenericAnimal's constructor registered the def-driven goal set (the
        // flying wander included); MC's adult table adds the float at
        // priority 3 — the one bespoke goal that does not ride the riding/
        // harness systems. Its tempt (HAPPY_GHAST_FOOD, snowballs) already
        // comes from the def's food list.
        m_goalSelector.AddGoal(3, std::make_unique<HappyGhastFloatGoal>(this));
    }

    void HappyGhast::AdultSetup() {
        // MC HappyGhast.adultGhastSetup: back to the goal set, brain stopped
        // and dropped. MC also swaps to the GhastMoveControl; the def's
        // FlyingMoveControl stands in for both ages here, so only the AI
        // layer swaps.
        m_brain.reset();
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        RegisterGoals();
        RegisterAdultGoals();
    }

    void HappyGhast::BabySetup() {
        // MC HappyGhast.babyGhastSetup: no goals at all — the ghastling is
        // pure brain (MC keeps FlyingMoveControl(180, true), which is what
        // the def already applied; the BabyFlyingPathNavigation variant is
        // not modelled).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        m_brain = std::make_unique<Brain>();
        HappyGhastAi::InitBrain(*this, *m_brain);
    }

    void HappyGhast::CustomServerAiStep() {
        // MC calls the setups from ageBoundaryReached; the port polls. The
        // brain itself (baby only, as in MC's customServerAiStep) is ticked
        // by Mob::ServerAiStep whenever m_brain exists.
        if (IsBaby() != m_wasBaby) {
            m_wasBaby = IsBaby();
            if (m_wasBaby) BabySetup();
            else           AdultSetup();
        }
    }

    void HappyGhast::UpdateBrainActivity() { HappyGhastAi::UpdateActivity(*this); }

} // namespace Game
