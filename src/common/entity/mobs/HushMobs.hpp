// File: src/common/entity/mobs/HushMobs.hpp
//
// The Hush's three mobs (docs/the-hush.md). Each rides a vanilla class under
// a different type id, texture and set of numbers — the way CamelHusk rides
// Camel — so the ported behaviour (the vex's charge, the warden's brain) is
// shared rather than copied:
//
//   Hushling     — MC Endermite's box and mesh on a skittish CREATURE goal
//                  set: wanders, freezes under a player's gaze, bolts when
//                  the player closes in. No attack.
//   EchoWraith   — MC Vex without an evoker: no owner, no bound origin, no
//                  limited life. Hunts players on its own.
//   SilentWarden — MC Warden with boss numbers (300 HP, a 12-damage sonic
//                  boom), never despawns, wakes when an echo core is broken
//                  (PlayerSession's break path) and carries a boss bar
//                  (server/level/SilentWardenBossBars).
//
// All three are built through MakeGenericMob's promotion switch, so both the
// server and the client factories construct the same classes.
#pragma once

#include "common/entity/mobs/AnimatedMobs.hpp"   // Warden
#include "common/entity/mobs/Monsters.hpp"       // Vex
#include "common/entity/ai/Goal.hpp"

namespace Game {

    // ── Hushling ───────────────────────────────────────────────────────────

    // MC PathfinderMob with the endermite's MAX_HEALTH 8 / MOVEMENT_SPEED
    // 0.25 and none of its bite. Goals mirror Endermite.registerGoals minus
    // MeleeAttackGoal, plus the flee (MC AvoidEntityGoal, Player.class) and
    // the freeze (HushlingFreezeGoal, engine-only).
    class Hushling : public PathfinderMob {
    public:
        explicit Hushling(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // A creature, not a monster: like MC Animal.removeWhenFarAway it
        // never despawns for distance — the creature cap bounds the herd.
        bool RemoveWhenFarAway(double) const override { return false; }

        // MC LivingEntity.isLookingAtMe(player, 0.025, scaleByDistance=true)
        // at the hushling's eye — the enderman's stare test, unchanged.
        bool IsLookedAtBy(LivingEntity& player);

        // The sculk drop. The loot generator resolves PURE items only (a
        // block item such as sculk has no Items:: row, so a JSON entry for
        // it is reported and dropped), so the table carries the 5 % echo
        // shard and the 0-1 sculk rides MC's Mob.dropCustomDeathLoot hook.
        void DropCustomDeathLoot(EntityLevel& level) override;

    protected:
        void RegisterGoals() override;
    };

    // The "freezes when you come close" half of the hushling. While a player
    // between kFleeDistance and kWatchDistance is looking at it, the hushling
    // stands stock-still and stares back for 40-80 ticks, then will not
    // freeze again for kCooldownTicks. Inside kFleeDistance the
    // higher-priority AvoidEntityGoal interrupts this one and it runs.
    // Engine-only (no MC counterpart); shaped like MC's Creaking freeze,
    // which is also a stare-gated stop.
    class HushlingFreezeGoal : public Goal {
    public:
        static constexpr float kFleeDistance  = 6.0f;
        static constexpr float kWatchDistance = 10.0f;
        static constexpr int   kCooldownTicks = 60;

        explicit HushlingFreezeGoal(Hushling* mob);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "HushlingFreezeGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        LivingEntity* FindWatcher() const;

        Hushling*     m_mob;
        LivingEntity* m_watcher = nullptr;
        int           m_holdTicks = 0;
        int           m_cooldownUntil = 0;   // in the mob's tickCount
    };

    // ── Echo Wraith ────────────────────────────────────────────────────────

    // MC Vex under EntityTypeId::EchoWraith. Vex.registerGoals minus the
    // VexCopyOwnerTargetGoal (there is no evoker to copy); the impulse move
    // control, the charge and the random drift are the vex's own. The
    // limited-life clock is never armed — that is EvokerSummonSpellGoal's
    // doing — so a wraith lives until killed or despawned like any monster.
    class EchoWraith : public Vex {
    public:
        explicit EchoWraith(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // ── Silent Warden ──────────────────────────────────────────────────────

    // MC Warden under EntityTypeId::SilentWarden: the whole brain (emerge,
    // roar, sniff, sonic boom, anger ladder) is inherited; only the numbers
    // change. Persistence is forced on, which is also what keeps Warden::Tick
    // refreshing the dig cooldown so it never burrows away.
    class SilentWarden : public Warden {
    public:
        explicit SilentWarden(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // MC SonicBoom's 10.0F for the vanilla warden; the boss hits harder.
        float SonicBoomDamage() const override { return 12.0f; }
    };

} // namespace Game
