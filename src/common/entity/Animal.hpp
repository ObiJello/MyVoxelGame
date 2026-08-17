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

#include "common/entity/Mob.hpp"

namespace Game {

    class AgeableMob : public PathfinderMob {
    public:
        AgeableMob(EntityTypeId type, EntityLevel* level);

        bool IsBaby() const override { return m_age < 0; }
        int  GetAge() const { return m_age; }
        void SetAge(int age) { m_age = age; }

        // MC ageUp — advance toward adulthood by `amount` ticks of growth.
        void AgeUp(int amount);

        float GetBbWidth()  const override;
        float GetBbHeight() const override;
        float GetEyeHeight() const override;

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
        void ResetLove() { m_inLove = 0; }

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

        // MC Animal.getBaseExperienceReward — 1..3, rolled per death.
        int GetXpReward() const;

        // MC Animal.checkAnimalSpawnRules — grass-ish surface and light > 8.
        static bool CheckAnimalSpawnRules(EntityLevel& level, const glm::ivec3& pos);

    protected:
        int m_inLove = 0;
    };

} // namespace Game
