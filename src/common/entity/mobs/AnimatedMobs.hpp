// File: src/common/entity/mobs/AnimatedMobs.hpp
//
// The mobs whose defining behaviour IS their animation state machine.
//
// MC drives every episodic animation from a Game::AnimationState timer started
// in `setupAnimationStates()` (client-side) or `onSyncedDataUpdated(DATA_POSE)`.
// The keyframes for all of them are already generated — what was missing was
// anything to start the timers, which is why a frog never croaked and a bat
// never flapped even though FROG_CROAK and BAT_FLYING were sitting in
// GeneratedAnimations.cpp.
//
// These four are the ones whose driving state this port can honestly produce:
//
//   Frog       pose CROAKING / USING_TONGUE / LONG_JUMPING, plus a purely
//              client-side swim idle. Croaking needed one goal (MC's Croak
//              behaviour); the rest follow from the pose being on the wire.
//   Camel      a free-running idle timer — no synched state at all.
//   Bat        resting vs flying, from MC's own flight AI, which is a plain
//              customServerAiStep with no brain and ports directly.
//   Armadillo  MC's four-state ARMADILLO_STATE machine plus the scare sensor.
//
// NOT here, and why: sniffer, warden, breeze and copper golem drive their
// timers from MC's BRAIN (Behavior/Activity/MemoryModule), which this port does
// not have. Their clips are extracted and their timer slots exist, so they
// animate the moment a brain — or a goal-shaped stand-in — sets the state.
// Inventing behaviour for them here would be worse than leaving them still.
#pragma once

#include "common/entity/mobs/GenericMobs.hpp"

#include <glm/glm.hpp>

namespace Game {

    // ── Frog ───────────────────────────────────────────────────────────────

    class Frog : public GenericAnimal {
    public:
        explicit Frog(EntityLevel* level);

        // MC Frog.isBaby is hardcoded false — frogs hatch from tadpoles, they
        // are never baby frogs, and the baby scale would shrink an adult.
        bool IsBaby() const override { return false; }

        void Tick() override;

        // MC Frog.onSyncedDataUpdated's DATA_POSE branch, verbatim.
        void OnPoseUpdated() override;

        // MC Frog.customServerAiStep's second half — FrogAi.updateActivity.
        void UpdateBrainActivity() override;

    protected:
        // MC Frog.updateWalkAnimation — the base multiplier is 25, not 4, AND
        // it drops to zero mid-jump so the frog holds the jump pose instead of
        // running the walk cycle through it.
        void UpdateWalkAnimation(float distance) override;
    };

    // ── Camel ──────────────────────────────────────────────────────────────

    class Camel : public GenericAnimal {
    public:
        // MC's two pose-transition lengths, in ticks. A camel is "in
        // transition" until the relevant one has elapsed, and the sit-down and
        // stand-up clips are exactly these long.
        static constexpr int kSitDownDuration = 40;
        static constexpr int kStandUpDuration = 52;

        // `type` so a camel husk keeps its own EntityTypeId (and therefore its
        // own texture, attributes and loot) while sharing MC's Camel class.
        explicit Camel(EntityLevel* level, EntityTypeId type = EntityTypeId::Camel);

        void SetupAnimationStates() override;
        void UpdateBrainActivity() override;
        void OnPoseUpdated() override;
        void Tick() override;

        // MC's whole sit state is ONE synched value: the game tick of the last
        // pose change, NEGATED while sitting. Everything else is derived.
        bool IsCamelSitting() const { return m_lastPoseChangeTick < 0; }
        int64_t GetPoseTime() const;
        bool IsInPoseTransition() const {
            return GetPoseTime() < (IsCamelSitting() ? kSitDownDuration : kStandUpDuration);
        }
        bool IsCamelVisuallySitting() const {
            return (GetPoseTime() < 0) != IsCamelSitting();
        }
        bool IsVisuallySittingDown() const {
            return IsCamelSitting() && GetPoseTime() < kSitDownDuration && GetPoseTime() >= 0;
        }
        // MC Camel.refuseToMove — a sitting or transitioning camel ignores
        // every movement instruction, which is why its brain gates strolling
        // on this.
        bool RefuseToMove() const { return IsCamelSitting() || IsInPoseTransition(); }

        void SitDown();
        void StandUp();
        void StandUpInstantly();

    private:
        void ResetLastPoseChangeTick(int64_t syncedPoseTickTime);

        // MC Camel.idleAnimationTimeout. No synched state and no server
        // involvement: every client runs its own timer, which is why two
        // players see the same camel sway at different moments in MC too.
        int m_idleAnimationTimeout = 0;

        // MC's LAST_POSE_CHANGE_TICK synched long. Not on the wire here: the
        // POSE already crosses it, and both sides recompute this from the tick
        // they saw the pose change on. The transitions are 40 and 52 ticks and
        // the pose arrives within 3, so the skew is invisible — where sending
        // a 64-bit game tick for one mob would not be.
        int64_t m_lastPoseChangeTick = 0;
    };

    // ── Bat ────────────────────────────────────────────────────────────────

    class Bat : public GenericMob {
    public:
        explicit Bat(EntityLevel* level);

        bool IsResting() const { return m_resting; }
        void SetResting(bool v) { m_resting = v; }

        // MC's DATA_ID_FLAGS bit 0. One bit in the shared animation-state byte
        // here; the meaning is private to this class on both sides.
        uint8_t GetAnimStateByte() const override { return m_resting ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_resting = (v & 1) != 0; }

        void Tick() override;
        void SetupAnimationStates() override;

    protected:
        void CustomServerAiStep() override;

    private:
        bool m_resting = true;      // MC's constructor calls setResting(true)
        bool m_hasTarget = false;
        glm::ivec3 m_targetPosition{0};
    };

    // ── Tadpole / Goat / Hoglin ────────────────────────────────────────────
    //
    // Three more mobs off the goal system. Each is only its brain plus whatever
    // state that brain reads — which is the point of having built the brain:
    // the third one costs a constructor.

    // PathfinderMob, not Animal: MC's Tadpole is an AbstractFish and gets
    // CreateMobAttributes, not CreateAnimalAttributes. Picking the wrong base
    // silently changes the attributes the mob is built with.
    class Tadpole : public GenericPathfinderMob {
    public:
        explicit Tadpole(EntityLevel* level);
        void UpdateBrainActivity() override;
    };

    class Goat : public GenericAnimal {
    public:
        explicit Goat(EntityLevel* level);
        void UpdateBrainActivity() override;
    };

    // Animal, not Monster: MC's Hoglin extends Animal and merely implements
    // Enemy, so it breeds and takes the animal attribute set.
    class Hoglin : public GenericAnimal {
    public:
        explicit Hoglin(EntityLevel* level);
        void UpdateBrainActivity() override;
    };

    // ── Warden ─────────────────────────────────────────────────────────────

    // Only the ATTACK animation. MC's warden drives roar, sniff, emerge and dig
    // from poses its BRAIN sets, and the sonic boom from an entity event only
    // the SonicBoom behaviour sends — none of which exist here, so wiring
    // handlers for them would make the audit go green while the animations
    // still never played. The attack is different: it hangs off doHurtTarget,
    // which MeleeAttackGoal already calls.
    class Warden : public GenericMonster {
    public:
        explicit Warden(EntityLevel* level) : GenericMonster(EntityTypeId::Warden, level) {}

        bool DoHurtTarget(Entity& target) override;
        void HandleEntityEvent(uint8_t id) override;
    };

    // ── Creaking ───────────────────────────────────────────────────────────

    // Attack only, for the same reason. The invulnerability flash needs a
    // creaking heart to be invulnerable TO, and the death animation needs that
    // heart to be destroyed; neither block exists.
    class Creaking : public GenericMonster {
    public:
        explicit Creaking(EntityLevel* level) : GenericMonster(EntityTypeId::Creaking, level) {}

        bool DoHurtTarget(Entity& target) override;
        void HandleEntityEvent(uint8_t id) override;
        void Tick() override;
        void SetupAnimationStates() override;

    private:
        // MC Creaking.attackAnimationRemainingTicks. Counted on both sides: the
        // server sets it in doHurtTarget, the client from entity event 4.
        int m_attackAnimationRemainingTicks = 0;
    };

    // ── Armadillo ──────────────────────────────────────────────────────────

    class Armadillo : public GenericAnimal {
    public:
        // MC Armadillo.ArmadilloState. The ids are MC's and they are the wire
        // encoding of the animation-state byte.
        enum class State : uint8_t {
            Idle = 0, Rolling = 1, Scared = 2, Unrolling = 3,
        };

        static bool  IsThreatened(State s) { return s != State::Idle; }
        static int   AnimationDuration(State s);
        static bool  ShouldHideInShell(State s, int64_t ticksInState);

        explicit Armadillo(EntityLevel* level);

        State GetState() const { return m_state; }
        void  SwitchToState(State s);

        bool IsScared() const { return m_state != State::Idle; }
        bool ShouldHideInShell() const { return ShouldHideInShell(m_state, m_inStateTicks); }
        bool ShouldSwitchToScaredState() const {
            return m_state == State::Rolling
                && m_inStateTicks > AnimationDuration(State::Rolling);
        }

        // MC Armadillo.canStayRolledUp. Leashes, riding and being ridden do not
        // exist here, so this is the two conditions that do.
        bool CanStayRolledUp() const { return !IsPanicking() && !IsInLiquid(); }

        // MC Armadillo.isScaredBy — what makes an armadillo curl up.
        bool IsScaredBy(const LivingEntity& other) const;

        void RollUp();
        void RollOut();

        // MC's DANGER_DETECTED_RECENTLY memory, as an expiry tick. The brain
        // stores a boolean with a time-to-live and ArmadilloBallUp reads the
        // REMAINING time to decide when to unroll, so an expiry tick is the
        // faithful shape, not a bool.
        int64_t DangerTicksRemaining() const;
        bool    DangerDetected() const { return DangerTicksRemaining() > 0; }

        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_state); }
        void    SetAnimStateByte(uint8_t v) override {
            SwitchToState(v <= 3 ? static_cast<State>(v) : State::Idle);
        }

        void Tick() override;
        void SetupAnimationStates() override;
        void HandleEntityEvent(uint8_t id) override;

        // MC Armadillo.hurtServer halves the damage (minus one) while rolled up.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    protected:
        void RegisterGoals() override;
        void CustomServerAiStep() override;

    private:
        State   m_state = State::Idle;
        int64_t m_inStateTicks = 0;
        int64_t m_dangerUntilTick = -1;

        // MC's client-only `peekReceivedClient`, set by entity event 64. It is
        // what stops the peek animation restarting every tick after the server
        // asks for one.
        bool m_peekReceivedClient = false;
    };

} // namespace Game
