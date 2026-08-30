// File: src/common/entity/ai/goals/BeeGoals.hpp
//
// MC animal/bee/Bee's goals.
//
// The combat trio (anger-gated, replacing the earlier stand-ins):
//
//   BeeAttackGoal              MeleeAttackGoal(1.4, true) that only runs
//                              while the bee is ANGRY and has not stung.
//   BeeHurtByOtherGoal         retaliation that alerts the swarm, sustained
//                              only while angry; alerted bees need the hurt
//                              bee's line of sight to the attacker.
//   BeeBecomeAngryTargetGoal   the isAngryAt-gated player hunt, dropped the
//                              moment the bee stings or the anger runs out.
//
// The flower/crop set (this wave):
//
//   BeeWanderGoal              the hive-less drift MC bees do between jobs.
//   BeePollinateGoal           find a BEE_ATTRACTIVE flower within 5, hover
//                              0.6 above it jittering ±1/3 block, and after
//                              400 successful hover ticks come away with
//                              nectar.
//   ValidateFlowerGoal         forget a saved flower that stopped existing.
//   BeeGrowCropGoal            the nectar dividend: while carrying nectar,
//                              1-in-30 ticks age-up a BEE_GROWABLES crop
//                              below, at most 10 per pollination.
//
// The hive goals stay unported, each needing beehive BLOCK ENTITIES:
// BeeEnterHiveGoal, BeeLocateHiveGoal, BeeGoToHiveGoal, ValidateHiveGoal —
// and with them BeeGoToKnownFlowerGoal (its trigger is the tiredness clock
// only the hive deposit cycle can restart).
//
// The Bee CLASS (AnimatedMobs.hpp) is owned elsewhere this wave, so the
// per-bee state MC keeps on the mob (savedFlowerPos, hasNectar, the flower
// cooldown, cropsGrown) lives in the shared BeeFlowerState below: the
// registration site creates ONE per bee and hands it to the four goals.
#pragma once

#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/world/block/BlockState.hpp"

#include <memory>
#include <optional>
#include <unordered_map>

namespace Game {

    class Bee;
    class JavaRandom;

    // MC Bee.attractsBees(BlockState) — the BEE_ATTRACTIVE tag (transcribed
    // from data/minecraft/tags/block/bee_attractive.json), minus waterlogged
    // states, with MC's sunflower special case (upper half only).
    bool AttractsBees(BlockState state);

    // The per-bee flower/pollination state MC keeps as Bee fields. One
    // instance is shared by the four goals below AND by the Bee itself (the
    // hasNectar bit rides the anim byte — coordinator wiring):
    //
    //   auto flowerState = std::make_shared<BeeFlowerState>();
    //   flowerState->remainingCooldownBeforeLocatingNewFlower =
    //       level->Random().NextInt(20, 60);            // MC's ctor roll
    //   ... AddGoal(...) with the same shared_ptr ...
    //
    // and per MC Bee.aiStep's server branch, once per server tick:
    //   if (state->remainingCooldownBeforeLocatingNewFlower > 0)
    //       --state->remainingCooldownBeforeLocatingNewFlower;
    // plus MC Bee.customServerAiStep:
    //   if (!state->hasNectar) ++state->ticksWithoutNectarSinceExitingHive;
    struct BeeFlowerState {
        // MC Bee.hasNectar (DATA_FLAGS_ID FLAG_HAS_NECTAR = 8) — bit 2 of the
        // bee's anim state byte on the wire (bits 0/1 are roll/stung).
        bool hasNectar = false;

        // MC Bee.savedFlowerPos.
        bool       hasSavedFlowerPos = false;
        glm::ivec3 savedFlowerPos{0};

        // MC Bee.remainingCooldownBeforeLocatingNewFlower (20..60 at spawn,
        // 200 after a pollination attempt ends).
        int remainingCooldownBeforeLocatingNewFlower = 0;

        // MC Bee.ticksWithoutNectarSinceExitingHive — the tiredness clock the
        // (skipped) hive cycle reads; kept so the wiring is ready.
        int ticksWithoutNectarSinceExitingHive = 0;

        // MC Bee.numCropsGrownSincePollination — resets only on hive exit,
        // which is the skipped piece; see BeeGrowCropGoal's cap note.
        int numCropsGrownSincePollination = 0;

        // MC Bee.dropFlower.
        void DropFlower(JavaRandom& rng);
    };

    // MC Bee.BeeAttackGoal.
    class BeeAttackGoal : public MeleeAttackGoal {
    public:
        BeeAttackGoal(Bee* bee, double speedModifier, bool trackTarget);

        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "BeeAttackGoal"; }

    private:
        Bee* m_bee;
    };

    // MC Bee.BeeHurtByOtherGoal.
    class BeeHurtByOtherGoal : public HurtByTargetGoal {
    public:
        explicit BeeHurtByOtherGoal(Bee* bee);

        bool CanContinueToUse() override;
        const char* Name() const override { return "BeeHurtByOtherGoal"; }

    protected:
        // MC alertOther: only bees answer, and only when the HURT bee still
        // sees the attacker.
        void AlertOther(Mob& other, LivingEntity& attacker) override;

    private:
        Bee* m_bee;
    };

    // MC Bee.BeeBecomeAngryTargetGoal — extends NearestAttackableTargetGoal
    // <Player>(bee, Player.class, 10, true, false, bee::isAngryAt).
    class BeeBecomeAngryTargetGoal : public NearestAttackableTargetGoal {
    public:
        explicit BeeBecomeAngryTargetGoal(Bee* bee);

        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "BeeBecomeAngryTargetGoal"; }

    private:
        bool BeeCanTarget() const;

        Bee* m_bee;
    };

    // MC Bee.BaseBeeGoal — the canBeeUse/canBeeContinueToUse split every
    // flower goal derives from: neither runs while the bee is angry.
    class BaseBeeGoal : public Goal {
    public:
        bool CanUse() override;
        bool CanContinueToUse() override;
        const char* Name() const override { return "BaseBeeGoal"; }

    protected:
        BaseBeeGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state);

        virtual bool CanBeeUse() = 0;
        virtual bool CanBeeContinueToUse() = 0;

        Bee* m_bee;
        std::shared_ptr<BeeFlowerState> m_state;
    };

    // MC Bee.BeeWanderGoal — the drift between jobs: 1-in-10 idle ticks,
    // pick a hover target ~8 blocks along the view direction. The hive-bias
    // half of findPos (steer home beyond the wander threshold) is skipped
    // with the hive goals — a hive-less MC bee takes exactly this branch.
    class BeeWanderGoal : public Goal {
    public:
        BeeWanderGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "BeeWanderGoal"; }

    private:
        std::optional<glm::dvec3> FindPos() const;

        Bee* m_bee;
        std::shared_ptr<BeeFlowerState> m_state;
    };

    // MC Bee.BeePollinateGoal.
    class BeePollinateGoal : public BaseBeeGoal {
    public:
        BeePollinateGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state);

        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "BeePollinateGoal"; }

        // MC isPollinating — read by wantsToEnterHive (hive cycle, skipped)
        // and by the roll-animation gate.
        bool IsPollinating() const { return m_pollinating; }

    protected:
        bool CanBeeUse() override;
        bool CanBeeContinueToUse() override;

    private:
        bool HasPollinatedLongEnough() const {
            return m_successfulPollinatingTicks > 400;
        }
        void SetWantedPos();
        float GetOffset() const;
        std::optional<glm::ivec3> FindNearbyFlower();

        int  m_successfulPollinatingTicks = 0;
        int  m_lastSoundPlayedTick = 0;
        bool m_pollinating = false;
        bool m_hasHoverPos = false;
        glm::dvec3 m_hoverPos{0.0};
        int  m_pollinatingTicks = 0;
        // MC's Long2LongOpenHashMap unreachableFlowerCache: packed BlockPos ->
        // game time until which the flower stays blacklisted.
        std::unordered_map<uint64_t, int64_t> m_unreachableFlowerCache;
    };

    // MC Bee.ValidateFlowerGoal — every 20..40 ticks, drop a saved flower
    // whose block no longer attracts bees.
    class ValidateFlowerGoal : public BaseBeeGoal {
    public:
        ValidateFlowerGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state);

        void Start() override;
        const char* Name() const override { return "ValidateFlowerGoal"; }

    protected:
        bool CanBeeUse() override;
        bool CanBeeContinueToUse() override { return false; }

    private:
        int     m_validateFlowerCooldown;
        int64_t m_lastValidateTick = -1;
    };

    // MC Bee.BeeGrowCropGoal — the nectar dividend. DEVIATION, documented:
    // MC additionally gates on isHiveValid(), and with the hive goals
    // skipped that gate would leave this goal permanently dead — it is
    // treated as satisfied, so a nectar-carrying bee grows crops until the
    // 10-crop cap (which, without the hive deposit that resets it, is a
    // per-bee lifetime cap).
    class BeeGrowCropGoal : public BaseBeeGoal {
    public:
        BeeGrowCropGoal(Bee* bee, std::shared_ptr<BeeFlowerState> state);

        void Tick() override;
        const char* Name() const override { return "BeeGrowCropGoal"; }

    protected:
        bool CanBeeUse() override;
        bool CanBeeContinueToUse() override { return CanBeeUse(); }
    };

} // namespace Game
