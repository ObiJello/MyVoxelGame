// File: src/common/entity/AnimationState.hpp
//
// MC net.minecraft.world.entity.AnimationState, ported, plus the slot table
// that lets a generic mob own one without a hand-written field per animation.
//
// WHAT THIS IS FOR. MC has two animation systems. The old one is `setupAnim`
// writing limb rotations by hand every frame; the new one is a declarative
// KeyframeAnimation played from a START TICK. A walk cycle is driven by the
// distance-walked accumulator and needs no timer, but everything episodic — a
// frog croaking, a camel's idle sway, an armadillo rolling up, a warden's roar —
// is a clip played once from the tick it began on. Without a timer those clips
// are dead data: the keyframes are baked, nothing ever plays them.
//
// MC keeps the timer on the ENTITY, not the model, and ticks it CLIENT-SIDE
// (`if (level().isClientSide()) setupAnimationStates();`). Two consequences that
// are easy to get wrong:
//
//   - The start tick is the CLIENT's tickCount, so it is never sent over the
//     wire. What crosses the wire is the state the animation is derived from
//     (the pose, a flag), and the client re-derives when to start and stop.
//   - `startIfStopped` and `start` are different: `start` restarts from zero
//     every time it is called, `startIfStopped` lets a running clip keep
//     running. Using the wrong one makes an animation either stutter at 20 Hz
//     or never replay.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>

namespace Game {

    class AnimationState {
    public:
        // MC's STOPPED sentinel. A real tickCount never reaches it.
        static constexpr int kStopped = std::numeric_limits<int>::min();

        void Start(int tickCount) { m_startTick = tickCount; }

        void StartIfStopped(int tickCount) {
            if (!IsStarted()) Start(tickCount);
        }

        // MC AnimationState.animateWhen — the one-liner most setupAnimationStates
        // are made of. Note it does NOT restart a running clip.
        void AnimateWhen(bool condition, int tickCount) {
            if (condition) StartIfStopped(tickCount);
            else           Stop();
        }

        void Stop() { m_startTick = kStopped; }

        bool IsStarted() const { return m_startTick != kStopped; }

        // MC AnimationState.fastForward — shifts the start tick backwards so the
        // clip appears to have already been running. The armadillo uses it to
        // drop straight into the middle of its peek rather than replaying the
        // roll-up every time it re-enters SCARED.
        void FastForward(int ticks, float timeScale) {
            if (IsStarted()) {
                m_startTick -= static_cast<int>(static_cast<float>(ticks) * timeScale);
            }
        }

        // MC AnimationState.getTimeInMillis, in seconds — the renderer's
        // KeyframeAnimation wants seconds and MC divides by 1000 immediately.
        float ElapsedSeconds(float ageInTicks) const {
            return (ageInTicks - static_cast<float>(m_startTick)) * 0.05f;
        }

        int StartTick() const { return m_startTick; }

    private:
        int m_startTick = kStopped;
    };

    // Every AnimationState any MC model reads, as one flat enum.
    //
    // MC declares these as named fields on nine different entity classes and
    // reads them through nine different RenderState subclasses. This port has
    // one generic mob class and one generic model, so the two sides have to
    // agree on a NAME — and a name shared by two mobs (camel and copper golem
    // both have an `idleAnimationState`, warden and creaking both an
    // `attackAnimationState`) is the same slot, because no entity has both.
    //
    // The order is not wire-visible: the timers never leave the client. It is
    // only a shared index between tools/gen_entity_models.py and the runtime, so
    // adding a slot means regenerating, not a protocol bump.
    enum class MobAnim : uint8_t {
        Attack = 0,
        Croak,
        Dash,
        Death,
        Digging,
        Emerge,
        FeelingHappy,
        Fly,
        Idle,
        Inhale,
        InteractionDropItem,
        InteractionDropNoItem,
        InteractionGetItem,
        InteractionGetNoItem,
        Invulnerability,
        Jump,
        LongJump,
        Peek,
        Rest,
        Rising,
        Roar,
        RollOut,
        RollUp,
        Scenting,
        Shoot,
        Sit,
        SitPose,
        SitUp,
        Slide,
        SlideBack,
        Sniff,
        Sniffing,
        SonicBoom,
        SwimIdle,
        Tongue,
        Count,
    };

    inline constexpr int kMobAnimCount = static_cast<int>(MobAnim::Count);
    static_assert(kMobAnimCount <= 64, "the render state packs 'started' into a uint64_t");

    // The block of timers a mob owns. Allocated on demand — nine mob types out
    // of ninety have any, and a fixed member would put 140 unused bytes on
    // every zombie in the world.
    using MobAnimationStates = std::array<AnimationState, kMobAnimCount>;

} // namespace Game
