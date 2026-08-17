// File: src/common/entity/mobs/Animals.hpp
//
// Cow, Pig, Sheep and Chicken.
//
// All four share the same goal skeleton — float, panic, breed, tempt, follow
// parent, stroll, look at player, look around — differing only in priorities,
// speeds and food. That is MC's structure and it is kept, because the
// priorities are where the personality is: a pig panics at 1.25 and a chicken
// at 1.4, so chickens visibly scatter faster.
#pragma once

#include <string_view>

#include "common/entity/Animal.hpp"

namespace Game {

    // MC Cow. MAX_HEALTH 10, MOVEMENT_SPEED 0.2.
    class Cow : public Animal {
    public:
        explicit Cow(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // MC Pig. MAX_HEALTH 10, MOVEMENT_SPEED 0.25.
    class Pig : public Animal {
    public:
        explicit Pig(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // MC Sheep. MAX_HEALTH 8, MOVEMENT_SPEED 0.23.
    //
    // The wool byte packs colour in the low four bits and "sheared" in bit 16,
    // exactly as MC's DATA_WOOL_ID does, so the renderer and the wire format
    // both stay one byte.
    class Sheep : public Animal {
    public:
        explicit Sheep(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        uint8_t GetColor() const { return m_woolData & 0x0F; }
        void    SetColor(uint8_t color);
        bool    IsSheared() const { return (m_woolData & 0x10) != 0; }
        void    SetSheared(bool sheared);
        uint8_t GetWoolData() const { return m_woolData; }
        void    SetWoolData(uint8_t v) { m_woolData = v; }

        // Grazing regrows wool, and grows a lamb toward adulthood.
        void OnEatBlock() override;

        // 0..1 head-down amount for the renderer, driven by the eat goal.
        float GetHeadEatPositionScale(float partialTick) const;
        float GetHeadEatAngleScale(float partialTick) const;

        void CustomServerAiStep() override;

        // The grazing goal, for the /sheepeat debug command.
        class EatBlockGoal* GetEatBlockGoal() const { return m_eatBlockGoal; }

        // MC Sheep.handleEntityEvent(10) — start the 40-tick graze animation.
        void HandleEntityEvent(uint8_t id) override;

        // MC Sheep.aiStep — counts the animation down CLIENT-side.
        void AiStep() override;

        static void CreateAttributes(AttributeMap& out);

        // MC SheepColorSpawnRules.getSheepColor — biome-dependent, so a
        // savanna sheep is usually brown and a snowy one usually black.
        static uint8_t RandomSpawnColor(class JavaRandom& rng, std::string_view biome);

        // MC Sheep.finalizeSpawn: roll the wool colour. Called for every spawn
        // reason, which is why spawn eggs give coloured sheep in vanilla.
        void FinalizeSpawn() override;

        // MC Sheep.mobInteract — the shears branch.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;

        // MC Sheep.readyForShearing: alive, unsheared, and NOT A LAMB.
        bool ReadyForShearing() const;

        // MC Sheep.shear — drop the wool and set the sheared flag.
        void Shear();

        // The wool item matching a DyeColor ordinal. Static because the death
        // drop needs it from the server's loot path as well as the shear does.
        static uint32_t WoolItemForColor(uint8_t color);

    protected:
        void RegisterGoals() override;

    private:
        uint8_t m_woolData = 0;
        int     m_eatAnimationTick = 0;
        class EatBlockGoal* m_eatBlockGoal = nullptr;
    };

    // MC Chicken. MAX_HEALTH 4, MOVEMENT_SPEED 0.25.
    class Chicken : public Animal {
    public:
        explicit Chicken(EntityLevel* level);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // MC Chicken.aiStep — the wing flap, which is both the animation and
        // the slow-fall: descending motion is scaled by 0.6 every tick, so a
        // chicken never takes fall damage.
        void AiStep() override;

        float GetFlap(float partialTick) const;
        float GetFlapSpeed(float partialTick) const;

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;

    private:
        float m_flap = 0.0f, m_oFlap = 0.0f;
        float m_flapSpeed = 0.0f, m_oFlapSpeed = 0.0f;
        float m_flapping = 1.0f;
        int   m_eggTime = 0;
    };

} // namespace Game
