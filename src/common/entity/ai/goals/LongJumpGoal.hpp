// File: src/common/entity/ai/goals/LongJumpGoal.hpp
//
// MC's long jump, ported: LongJumpToRandomPos + LongJumpToPreferredBlock +
// LongJumpMidJump + LongJumpUtil, collapsed into one goal.
//
// WHY ONE GOAL RATHER THAN THREE. MC splits it because its brain runs an
// ACTIVITY containing two behaviours: LongJumpToRandomPos searches and launches,
// then LongJumpMidJump (priority 0) takes over the moment the LONG_JUMP_MID_JUMP
// memory appears — which only the first one ever sets. They are sequential
// phases of a single act that can never overlap, and expressing them as a
// four-phase goal is the same machine with the handoff made explicit instead of
// mediated by a memory this port does not have.
//
// THE MATH IS NOT NEGOTIABLE. `CalculateJumpVectorForAngle` solves the ballistic
// trajectory for a fixed launch angle against the mob's own gravity attribute,
// then walks the arc checking for collisions. Get a term wrong and the failure
// is not "the jump looks slightly off" — it is a frog that launches through a
// wall, or one that computes a negative v0 for every candidate and therefore
// never jumps at all, silently.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>
#include <optional>
#include <vector>

namespace Game {

    class Mob;

    // MC LongJumpUtil.calculateJumpVectorForAngle.
    //
    // Returns the launch velocity that lands the mob on `targetPos` at
    // `angleDegrees` above horizontal, or nothing when no such trajectory
    // exists (negative v0², faster than `maxJumpVelocity`, or the arc clips
    // geometry when `checkCollision`).
    std::optional<glm::dvec3> CalculateJumpVectorForAngle(
        Mob& mob, const glm::dvec3& targetPos, float maxJumpVelocity,
        int angleDegrees, bool checkCollision);

    class LongJumpGoal : public Goal {
    public:
        // `acceptableLandingSpot` is MC's BiPredicate — the frog's version also
        // rejects fluids and accepts lily pads outright, so it cannot be folded
        // into the default.
        using LandingSpotTest = bool (*)(Mob&, const glm::ivec3&);

        struct Config {
            // MC's UniformInt timeBetweenLongJumps. The frog's is (100, 140).
            int   cooldownMin = 100;
            int   cooldownMax = 140;
            int   maxHeight = 2;
            int   maxWidth = 4;
            // Multiplies JUMP_STRENGTH to give the velocity ceiling. The frog's
            // 3.5714288 against the default 0.42 is exactly 1.5 blocks/tick.
            float maxJumpVelocityMultiplier = 3.5714288f;

            // MC LongJumpToPreferredBlock. Empty = the plain random variant.
            const BlockID* preferredBlocks = nullptr;
            int            preferredBlockCount = 0;
            float          preferredBlocksChance = 0.0f;

            LandingSpotTest acceptableLandingSpot = nullptr;   // null = MC's default

            // When the BRAIN drives this, the cooldown lives in the
            // LONG_JUMP_COOLDOWN_TICKS memory and the activity is gated on it.
            // Keeping the goal's own cooldown as well would double it — MC's
            // LongJumpToRandomPos has no internal cooldown at all.
            bool ownsCooldown = true;
        };

        LongJumpGoal(Mob* mob, const Config& config);

        const char* Name() const override { return "LongJumpGoal"; }

        // Every phase counts ticks, and the 40-tick crouch before launch is a
        // real timing the player sees. On the 2-tick goal cadence it would
        // become 80.
        bool RequiresUpdateEveryTick() const override { return true; }

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;

        // MC LongJumpToRandomPos.defaultAcceptableLandingSpot — solid ground
        // below, and a path type this mob does not avoid.
        static bool DefaultAcceptableLandingSpot(Mob& mob, const glm::ivec3& target);

    private:
        // MC LongJumpToRandomPos.PossibleJump.
        struct PossibleJump {
            glm::ivec3 targetPos;
            int        weight;
        };

        enum class Phase : uint8_t { Searching, Preparing, MidJump };

        std::optional<PossibleJump> GetJumpCandidate();
        void PickCandidate();
        bool IsAcceptableLandingPosition(const glm::ivec3& target) const;
        std::optional<glm::dvec3> CalculateOptimalJumpVector(const glm::dvec3& targetPos);
        bool IsPreferredBlockBelow(const glm::ivec3& target) const;
        int  SampleCooldown() const;

        Mob*   m_mob;
        Config m_cfg;

        std::vector<PossibleJump> m_candidates;
        // MC LongJumpToPreferredBlock's second list: candidates rejected only
        // for not being a preferred block, kept as the fallback so a frog that
        // wanted a lily pad still jumps somewhere when there is none.
        std::vector<PossibleJump> m_notPreferred;
        bool m_wantingPreferred = false;

        Phase      m_phase = Phase::Searching;
        glm::dvec3 m_initialPosition{0.0};
        // MC's LOOK_TARGET memory. Held for the whole crouch, because the body
        // only follows a head that is HOLDING a direction — see Tick.
        glm::dvec3 m_lookTarget{0.0};
        std::optional<glm::dvec3> m_chosenJump;
        int     m_findJumpTries = 0;
        int     m_prepareTicks = 0;
        int64_t m_startTick = 0;
        int64_t m_cooldownUntilTick = -1;
    };

} // namespace Game
