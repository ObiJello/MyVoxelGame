// File: src/common/entity/Animal.hpp
//
// MC net.minecraft.world.entity.{AgeableMob, animal.Animal}.
//
// Age is one signed counter and its sign is the whole state machine:
//   age <  0   baby, counting UP toward 0 (so -24000 is a newborn)
//   age == 0   adult, able to breed
//   age >  0   adult on breeding cooldown, counting DOWN toward 0
// One field, three meanings — which is why FollowParentGoal tests `age < 0`
// and BreedGoal tests `age == 0`, and why "is baby" is not a bool.
//
// Animals never despawn (RemoveWhenFarAway is false). That is deliberate in MC
// and load-bearing here: a world with no entity persistence would otherwise
// lose every animal the moment a player walked away, and the natural spawner
// only refills chunks on generation.
#pragma once

#include "common/core/EntityRef.hpp"

#include "common/entity/Mob.hpp"
#include "common/entity/SpawnReason.hpp"

namespace Game {

    class AgeableMob : public PathfinderMob {
    public:
        AgeableMob(EntityTypeId type, EntityLevel* level);

        bool IsBaby() const override { return m_age < 0; }
        int  GetAge() const { return m_age; }
        void SetAge(int age) { m_age = age; }

        // MC ageUp(seconds, forced) — advance toward adulthood by `seconds`
        // seconds of growth. The forced flag is the feeding path: forced
        // growth accumulates in forcedAge, and MC's own quirk applies — an
        // animal fed to adulthood lands on a breeding cooldown equal to the
        // growth it was force-fed.
        void AgeUp(int seconds, bool forced = false);

        // MC AgeableMob.getSpeedUpSecondsWhenFeeding — 10% of the remaining
        // ticks, expressed in seconds.
        static int GetSpeedUpSecondsWhenFeeding(int ticksUntilAdult) {
            return static_cast<int>(static_cast<float>(ticksUntilAdult / 20) * 0.1f);
        }

        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        void AiStep() override;

        // Breeding cooldown after producing a child.
        static constexpr int kParentAgeAfterBreeding = 6000;
        static constexpr int kBabyStartAge = -24000;

    protected:
        int m_age = 0;
        // MC forcedAge / forcedAgeTimer, used by the growth-acceleration path.
        int m_forcedAge = 0;
        int m_forcedAgeTimer = 0;
    };

    class Animal : public AgeableMob {
    public:
        Animal(EntityTypeId type, EntityLevel* level);

        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        // Animals are never removed for being far away — see the header note.
        bool RemoveWhenFarAway(double) const override { return false; }

        int GetAmbientSoundInterval() const override { return 120; }

        // ── Breeding ───────────────────────────────────────────────────────
        bool IsInLove() const { return m_inLove > 0; }
        void SetInLove(Entity* cause);
        // MC Animal.getLoveCause, reduced to the entity id (the player may
        // log off mid-courtship). -1 = no player fed this animal.
        // Kept for the gameplay code that credits the feeder (breeding XP and
        // advancements): it resolves the identity to a live session id, or -1
        // when the feeder is gone. Storage is the EntityRef, so the identity
        // survives a save even while this reads -1.
        int32_t GetLoveCauseId() const;
        // MC Animal.resetLove clears the timer AND the remembered feeder.
        void ResetLove() { m_inLove = 0; m_loveCauseRef.Clear(); }

        // ── Restore-from-save setters ───────────────────────────────────────
        //
        // SetInLove(Entity*) hardcodes 600 ticks AND broadcasts entity event
        // 18, which is the heart particles. Restoring a saved love timer
        // through it would reset every animal's countdown to full and spray
        // hearts across the world on load, so loading needs a plain setter.
        // The cause entity is deliberately not restored — see the omission
        // list: it is a per-session id that cannot be resolved.
        void SetInLoveTicks(int ticks) { m_inLove = ticks; }
        // The breeding partner, as an identity. It was a session entity id,
        // which cannot survive a save.
        void SetLoveCauseUuid(const Uuid& uuid) { m_loveCauseRef.SetUnresolved(uuid); }
        const EntityRef& LoveCauseRef() const { return m_loveCauseRef; }
        int  GetInLoveTicks() const { return m_inLove; }

        // AgeUp(seconds, forced) is the only writer of m_forcedAge today and it
        // moves m_age as a side effect, so restoring the two independently
        // needs this.
        void SetForcedAge(int age) { m_forcedAge = age; }
        int  GetForcedAge() const { return m_forcedAge; }

        // MC Animal.canFallInLove — the turtle (egg) and horse family narrow
        // it; the base only refuses while already courting.
        virtual bool CanFallInLove() const { return m_inLove <= 0; }

        // MC Animal.mobInteract — the shared feeding path: food ages a baby
        // up by 10% and courts an adult. Every animal's own mobInteract
        // (sheep shears, wolf bone, cat fish) falls through to this.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Mob.usePlayerItem — consume one of the held stack. The creative
        // count restore is IntegratedServer::HandleInteract's, matching MC's
        // hasInfiniteMaterials handling in ItemStack.consume.
        static void UsePlayerItem(ItemStack& held) {
            held.count -= 1;
            if (held.count <= 0) held.Clear();
        }

        // MC Animal.canMate — same species, both in love. The species test is
        // exact rather than "same base class", which is why a cow and a
        // mooshroom do not breed in MC.
        virtual bool CanMate(const Animal& other) const;

        // MC Animal.spawnChildFromBreeding: create the baby, reset both
        // parents, and drop experience.
        virtual void SpawnChildFromBreeding(Animal& partner);

        // Each concrete animal makes its own kind of baby.
        virtual std::unique_ptr<Animal> CreateBaby() = 0;

        // MC Animal.isFood — what TemptGoal and breeding accept.
        virtual bool IsFood(uint32_t itemId) const { return false; }

        // MC Sheep.ate — what happens when EatBlockGoal completes. Sheep
        // regrow wool and lambs grow up; every other animal ignores it. The
        // goal calls this rather than special-casing sheep, so grazing can be
        // given to another animal without touching the goal.
        virtual void OnEatBlock() {}

        void AiStep() override;

        // MC Animal.handleEntityEvent — 18 = the 7-heart breeding burst
        // (fed into love, and again at breed completion). Also answers this
        // port's kEntityEventLoveHeart (one periodic courtship heart — see
        // EntityLevel.hpp for why the cadence travels as a byte).
        void HandleEntityEvent(uint8_t id) override;

        // MC Animal.getBaseExperienceReward (Animal.java:123) — 1..3, rolled
        // per death.
        int GetXpReward() const override;

        // MC Animal.checkAnimalSpawnRules — ANIMALS_SPAWNABLE_ON surface and
        // light > 8 (unless the spawn reason ignores light).
        static bool CheckAnimalSpawnRules(EntityLevel& level, SpawnReason reason,
                                          const glm::ivec3& pos);

        // MC Animal.isBrightEnoughToSpawn — raw brightness (amount 0) > 8.
        // Public rather than protected because half the per-type spawn rules
        // in SpawnPlacements borrow it.
        static bool IsBrightEnoughToSpawn(EntityLevel& level, const glm::ivec3& pos);

    protected:
        int m_inLove = 0;
        // MC Animal.loveCause — the player who fed this animal into love, as
        // an entity id (the player can log off mid-courtship). Read by the
        // breeding-XP award; -1 when love came without a player (BreedGoal
        // pairing the other parent, /summon).
        mutable EntityRef m_loveCauseRef;   // mutable: GetLoveCauseId resolves lazily
    };

} // namespace Game
