// File: src/common/entity/mobs/AnimatedMobs.cpp
#include "common/entity/mobs/AnimatedMobs.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/LongJumpGoal.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/brain/CamelAi.hpp"
#include "common/entity/ai/brain/GoatAi.hpp"
#include "common/entity/ai/brain/HoglinAi.hpp"
#include "common/entity/ai/brain/TadpoleAi.hpp"
#include "common/entity/ai/brain/FrogAi.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

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
        // MC Camel.tick: a sitting camel that ends up in water stands straight
        // back up, because the sitting hitbox would drown it.
        if (m_level && !m_level->IsClientSide() && IsCamelSitting() && IsInWater()) {
            StandUpInstantly();
        }
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
            // Dashing needs a rider, which does not exist — MC's condition
            // evaluated, not stubbed.
            Anim(MobAnim::Dash).AnimateWhen(false, tickCount);
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

    // ══ Warden ═════════════════════════════════════════════════════════════

    namespace {
        // MC's shared "this mob just landed a melee hit" entity event. Warden
        // and creaking both use it to start their attack animation on every
        // client that can see them.
        constexpr uint8_t kEventMobAttack = 4;
    }

    bool Warden::DoHurtTarget(Entity& target) {
        // MC Warden.doHurtTarget broadcasts BEFORE delegating, so the animation
        // starts on the same tick the damage lands rather than the next one.
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
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
        GenericMonster::HandleEntityEvent(id);
    }

    // ══ Creaking ═══════════════════════════════════════════════════════════

    bool Creaking::DoHurtTarget(Entity& target) {
        // MC refuses to swing at anything that is not alive, and the animation
        // is inside that guard — a creaking does not windmill at an item frame.
        if (!dynamic_cast<LivingEntity*>(&target)) return false;
        m_attackAnimationRemainingTicks = 15;
        if (m_level) m_level->BroadcastEntityEvent(*this, kEventMobAttack);
        return GenericMonster::DoHurtTarget(target);
    }

    void Creaking::HandleEntityEvent(uint8_t id) {
        if (id == kEventMobAttack) {
            m_attackAnimationRemainingTicks = 15;
            return;
        }
        GenericMonster::HandleEntityEvent(id);
    }

    void Creaking::Tick() {
        GenericMonster::Tick();
        if (m_attackAnimationRemainingTicks > 0) --m_attackAnimationRemainingTicks;
    }

    void Creaking::SetupAnimationStates() {
        // MC Creaking.setupAnimationStates, minus the two lines that need a
        // creaking heart: invulnerability fires only while the heart protects
        // it, and the death clip only once the heart is destroyed.
        Anim(MobAnim::Attack).AnimateWhen(m_attackAnimationRemainingTicks > 0, tickCount);
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

    namespace {

        // MC ArmadilloAi.ArmadilloBallUp, as a Goal.
        //
        // MC puts it alone in the PANIC activity, which REPLACES the idle
        // activity wholesale — so as a goal it takes priority 0 and every
        // movement flag, which is the same "nothing else runs" effect.
        class ArmadilloBallUpGoal : public Goal {
        public:
            explicit ArmadilloBallUpGoal(Armadillo* mob) : m_mob(mob) {
                SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
            }

            const char* Name() const override { return "ArmadilloBallUpGoal"; }
            bool RequiresUpdateEveryTick() const override { return true; }

            bool CanUse() override {
                // MC: PANIC needs DANGER_DETECTED_RECENTLY present and
                // IS_PANICKING absent; the behaviour itself needs onGround.
                return m_mob->DangerDetected() && !m_mob->IsPanicking()
                       && m_mob->onGround;
            }

            bool CanContinueToUse() override {
                return Armadillo::IsThreatened(m_mob->GetState());
            }

            void Start() override { m_mob->RollUp(); }

            void Stop() override {
                if (!m_mob->CanStayRolledUp()) m_mob->RollOut();
            }

            void Tick() override {
                if (m_nextPeekTimer > 0) --m_nextPeekTimer;

                if (m_mob->ShouldSwitchToScaredState()) {
                    m_mob->SwitchToState(Armadillo::State::Scared);
                    return;
                }

                const Armadillo::State state = m_mob->GetState();
                const int64_t dangerTicks = m_mob->DangerTicksRemaining();
                const bool dangerIsAround = dangerTicks > kDangerThreshold;
                if (dangerIsAround != m_dangerWasAround) {
                    m_nextPeekTimer = PickNextPeekTimer();
                }
                m_dangerWasAround = dangerIsAround;

                if (state == Armadillo::State::Scared) {
                    if (m_nextPeekTimer == 0 && m_mob->onGround && dangerIsAround) {
                        // MC broadcasts entity event 64; the client turns that
                        // into one peek, which is why the armadillo pokes its
                        // head out at intervals instead of continuously.
                        if (m_mob->Level()) {
                            m_mob->Level()->BroadcastEntityEvent(*m_mob, 64);
                        }
                        m_nextPeekTimer = PickNextPeekTimer();
                    }
                    if (dangerTicks
                        < Armadillo::AnimationDuration(Armadillo::State::Unrolling)) {
                        m_mob->SwitchToState(Armadillo::State::Unrolling);
                    }
                } else if (state == Armadillo::State::Unrolling
                           && dangerTicks
                              > Armadillo::AnimationDuration(Armadillo::State::Unrolling)) {
                    m_mob->SwitchToState(Armadillo::State::Scared);
                }
            }

        private:
            // MC ArmadilloBallUp.DANGER_DETECTED_RECENTLY_DANGER_THRESHOLD.
            static constexpr int kDangerThreshold = 75;

            int PickNextPeekTimer() const {
                EntityLevel* level = m_mob->Level();
                const int jitter = level ? level->Random().NextInt(100, 400) : 250;
                return Armadillo::AnimationDuration(Armadillo::State::Scared) + jitter;
            }

            Armadillo* m_mob;
            int  m_nextPeekTimer = 0;
            bool m_dangerWasAround = false;
        };

    } // namespace

    Armadillo::Armadillo(EntityLevel* level)
        : GenericAnimal(EntityTypeId::Armadillo, level) {
        RegisterGoals();
    }

    void Armadillo::RegisterGoals() {
        // GenericAnimal's constructor already registered MC's shared animal
        // set; this only adds the one goal that is the armadillo's own.
        m_goalSelector.AddGoal(0, std::make_unique<ArmadilloBallUpGoal>(this));
    }

    void Armadillo::SwitchToState(State s) {
        if (m_state == s) return;
        m_state = s;
        // MC resets inStateTicks in onSyncedDataUpdated, i.e. on BOTH sides,
        // which is what keeps the client's shouldHideInShell in step with the
        // animation it is playing.
        m_inStateTicks = 0;
    }

    int64_t Armadillo::DangerTicksRemaining() const {
        if (m_dangerUntilTick < 0) return 0;
        return std::max<int64_t>(0, m_dangerUntilTick - tickCount);
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
        if (GetLastHurtByMob() == &other) return true;
        if (other.IsPlayer()) {
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
            m_dangerUntilTick = tickCount + 80;
            if (CanStayRolledUp()) RollUp();
        }
        return hurt;
    }

    void Armadillo::CustomServerAiStep() {
        GenericAnimal::CustomServerAiStep();
        if (!m_level) return;

        // MC SensorType.ARMADILLO_SCARE_DETECTED:
        //   MobSensor(5, Armadillo::isScaredBy, Armadillo::canStayRolledUp,
        //             DANGER_DETECTED_RECENTLY, 80)
        // A sensor with scanRate 5 and a memory with an 80-tick expiry — and
        // note the readyTest ERASES the memory rather than merely not setting
        // it, so an armadillo that walks into water unrolls immediately.
        if (tickCount % 5 == 0) {
            if (!CanStayRolledUp()) {
                m_dangerUntilTick = -1;
            } else {
                AABB scan = GetAABB();
                scan.min -= glm::vec3(7.0f, 2.0f, 7.0f);
                scan.max += glm::vec3(7.0f, 2.0f, 7.0f);

                std::vector<Entity*> nearby;
                m_level->GetEntitiesInBox(scan, this, nearby);
                for (Entity* e : nearby) {
                    auto* living = dynamic_cast<LivingEntity*>(e);
                    if (living && IsScaredBy(*living)) {
                        m_dangerUntilTick = tickCount + 80;
                        break;
                    }
                }
            }
        }

        // MC ArmadilloAi.ARMADILLO_ROLLING_OUT, an IDLE-activity behaviour at
        // priority 0: with the danger memory gone, a scared armadillo unrolls.
        if (!DangerDetected() && IsScared()
            && m_state != State::Unrolling && m_state != State::Rolling) {
            RollOut();
        }
    }

} // namespace Game
