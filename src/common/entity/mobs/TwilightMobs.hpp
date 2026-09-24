// File: src/common/entity/mobs/TwilightMobs.hpp
//
// The Twilight Forest's first creatures (docs/mod-ports.md, pass one), ported
// from the mod's own classes (mods_reference/twilightforest, entity/passive
// and entity/monster). Numbers come from each class's registerAttributes and
// TFEntities' builder rows; goal sets are the mod's registerGoals rebuilt
// from the engine's goal classes, with the mod goals that have no engine
// counterpart named at their site.
//
//   Deer          — Animal; skittish (flees players inside 16 blocks), breeds
//                   on wheat or apples.
//   Boar          — Animal; the pig's goal set, breeds on carrots, potatoes,
//                   beetroot.
//   Bighorn       — MC Sheep's goal set, grazing, fleece colours and
//                   shearing under its own type id.
//   TinyBird      — FlyingBird: sits on leaves and sturdy tops, takes off
//                   when a player comes close, flutters about Bat-style and
//                   lands again. Four colour variants.
//   Raven         — FlyingBird: the tiny bird's flight, spooked only by harm.
//   Penguin       — Bird: waddles, breeds on fish.
//
// The rest of the TF biome-spawner creatures (squirrel, dwarf rabbit, the
// wolves, king spider, mosquito swarm, skeleton druid, yeti) live in
// TwilightCreatures.hpp; MakeTwilightMob builds both files' types.
//   Kobold        — Monster; melee with a leap, panics when hurt.
//   Redcap        — Monster; melee.
//
// None has a MobDef row (no MC class for the def generator to read), so
// they are built through MakeGenericMob's promotion switch, BEFORE the def
// check — the same path the Hush's mobs take (HushMobs.hpp).
#pragma once

#include "common/entity/Animal.hpp"
#include "common/entity/ModMobNbt.hpp"
#include "common/entity/Monster.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace Game {

    class EatBlockGoal;

    // The Twilight Forest creatures' factory — MakeGenericMob's promotion
    // switch for this file's types (and TwilightCreatures'). Null for any
    // other type.
    std::unique_ptr<Mob> MakeTwilightMob(EntityTypeId type, EntityLevel* level);

    // ── Sheep-shaped grazers ───────────────────────────────────────────────

    // MC Sheep's grazing half, for the mod animals that register an
    // EatBlockGoal (TF Bighorn inherits Sheep's; the Aether's Sheepuff has a
    // copy). The goal broadcasts entity event 10, the client counts the
    // 40-tick head dip down itself (MC Sheep.handleEntityEvent / aiStep), and
    // the renderer reads the two head scales exactly as it does the sheep's.
    // Shared with AetherMobs' Sheepuff.
    class GrazingAnimal : public Animal {
    public:
        GrazingAnimal(EntityTypeId type, EntityLevel* level);

        void HandleEntityEvent(uint8_t id) override;
        void AiStep() override;
        void CustomServerAiStep() override;

        // MC Sheep.ate: a lamb grazes itself 60 seconds older. The wool half
        // waits on shearing.
        void OnEatBlock() override;

        // MC Sheep.getHeadEatPositionScale / getHeadEatAngleScale.
        float GetHeadEatPositionScale(float partialTick) const;
        float GetHeadEatAngleScale(float partialTick) const;

    protected:
        // Adds the EatBlockGoal at `priority` and keeps the pointer for the
        // server-side counter (MC keeps the same field).
        void AddEatBlockGoal(int priority);

    private:
        EatBlockGoal* m_eatBlockGoal = nullptr;
        int           m_eatAnimationTick = 0;
    };

    // ── Deer ───────────────────────────────────────────────────────────────

    // TF passive/Deer. MAX_HEALTH 10, MOVEMENT_SPEED 0.2.
    class Deer : public Animal {
    public:
        explicit Deer(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // TFItemTags.DEER_TEMPT_ITEMS: #c:crops/wheat, apple, shika_senbei
        // (the last is a TF item this engine does not have yet).
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

    protected:
        void RegisterGoals() override;
    };

    // ── Boar ───────────────────────────────────────────────────────────────

    // TF passive/Boar. MAX_HEALTH 10, MOVEMENT_SPEED 0.25.
    class Boar : public Animal {
    public:
        explicit Boar(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // TFItemTags.BOAR_TEMPT_ITEMS: #c:crops/carrot, potato, beetroot.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

    protected:
        void RegisterGoals() override;
    };

    // ── Bighorn sheep ──────────────────────────────────────────────────────

    // TF passive/Bighorn extends MC Sheep: Sheep.createAttributes (MAX_HEALTH
    // 8, MOVEMENT_SPEED 0.23) and Sheep.registerGoals, both inherited — and
    // with them MC Sheep's fleece: a DyeColor plus the sheared bit (the
    // engine sheep's wool byte, bits 0-3 colour, bit 4 sheared, carried as
    // the wire variant), shearing with shears, regrowth on grazing, and the
    // colour mix when two bighorns breed. Only the spawn roll is TF's own
    // (getRandomFleeceColor: nextBoolean() ? BROWN : any of the sixteen).
    class Bighorn : public GrazingAnimal {
    public:
        explicit Bighorn(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        // ItemTags.SHEEP_FOOD (wheat), inherited from Sheep.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;
        // Bighorn.getBreedOffspring: the lamb's colour is
        // DyeColor.getMixedColor(parent, partner) — needs the partner, which
        // the engine hands SpawnChildFromBreeding and not CreateBaby.
        void SpawnChildFromBreeding(Animal& partner) override;

        // Bighorn.getWalkTargetValue: grass OR podzol below scores 10.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        uint8_t GetColor() const { return m_woolData & 0x0F; }
        void    SetColor(uint8_t color) {
            m_woolData = static_cast<uint8_t>((m_woolData & 0xF0) | (color & 0x0F));
        }
        bool    IsSheared() const { return (m_woolData & 0x10) != 0; }
        void    SetSheared(bool sheared) {
            m_woolData = sheared ? static_cast<uint8_t>(m_woolData | 0x10)
                                 : static_cast<uint8_t>(m_woolData & ~0x10);
        }
        uint8_t GetVariantByte() const override { return m_woolData; }
        void    SetVariantByte(uint8_t v) override { m_woolData = v; }

        // Bighorn.finalizeSpawn: super (the sheep's biome colour, overwritten)
        // then getRandomFleeceColor.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Sheep.mobInteract (shears), readyForShearing, shear.
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        bool ReadyForShearing() const { return IsAlive() && !IsSheared() && !IsBaby(); }
        void Shear();

        // MC Sheep.ate: regrow the fleece, and the lamb's 60 seconds.
        void OnEatBlock() override;

        // The sheep table's wool pool (loot_table/entities/bighorn_sheep/
        // <colour>: one wool of the fleece colour, unsheared only) is a block
        // item, which the baked loot tables cannot carry — the mutton pool
        // rides bighorn_sheep.json, the wool this hook.
        void DropCustomDeathLoot(EntityLevel& level) override;

        // MC Sheep.addAdditionalSaveData: "Color" (byte), "Sheared" (bool).
        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        uint8_t  m_woolData = 12;           // DyeColor.BROWN until finalizeSpawn rolls
        Bighorn* m_breedPartner = nullptr;  // set only inside SpawnChildFromBreeding
    };

    // ── Birds ──────────────────────────────────────────────────────────────

    // TF passive/Bird extends Animal: the flap counters every TF bird carries
    // for its renderer (BirdRenderer.extractRenderState reads them), run on
    // both sides from aiStep, and the slowed fall. Birds do not breed unless
    // the subclass says so (Bird.isFood false, getBreedOffspring null).
    class TFBird : public Animal {
    public:
        TFBird(EntityTypeId type, EntityLevel* level);

        bool IsFood(uint32_t) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }

        // Bird.aiStep (flap counters + the slowed fall) — both sides.
        void AiStep() override;

        // BirdRenderer.extractRenderState: flap = lerp(lastFlapLength,
        // flapLength), flapSpeed = lerp(lastFlapIntensity, flapIntensity).
        float GetFlap(float partialTick) const;
        float GetFlapIntensity(float partialTick) const;

    private:
        float m_flapLength = 0.0f;
        float m_flapIntensity = 0.0f;
        float m_lastFlapIntensity = 0.0f;
        float m_lastFlapLength = 0.0f;
        float m_flapSpeed = 1.0f;
    };

    // TF passive/FlyingBird extends Bird — the tiny bird's and the raven's
    // shared flight. A landed bird walks with the goal set; customServerAiStep
    // decides when it takes off and, while airborne, steers it Bat-style
    // toward a random nearby point until it finds a leaf or sturdy block to
    // land on. The landed flag is DATA_BIRDFLAGS bit 0, carried in the anim
    // byte. Never a baby, never pushed, never rides.
    class FlyingBird : public TFBird {
    public:
        FlyingBird(EntityTypeId type, EntityLevel* level);

        // FlyingBird.isBaby: always false.
        bool IsBaby() const override { return false; }
        // FlyingBird.isPushable / doPush / pushEntities.
        bool IsPushable() const override { return false; }
        // FlyingBird.canRide.
        bool CanRide(const Entity&) const override { return false; }

        bool IsBirdLanded() const { return m_landed; }
        void SetBirdLanded(bool landed) { m_landed = landed; }
        uint8_t GetAnimStateByte() const override { return m_landed ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_landed = (v & 1) != 0; }

        // FlyingBird.tick: level out while flying.
        void Tick() override;
        // FlyingBird.customServerAiStep: take-off, Bat-style flight, landing.
        void CustomServerAiStep() override;

        // FlyingBird.getWalkTargetValue: leaves 200, logs 15, dirt-likes 9,
        // else brightness.
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

    protected:
        // FlyingBird.registerGoals (the tempt tag is #c:seeds for both the
        // tiny bird and the raven: TFItemTags TINY_BIRD / RAVEN_TEMPT_ITEMS).
        void RegisterFlyingBirdGoals();
        // FlyingBird.isSpooked (abstract).
        virtual bool IsSpooked() = 0;

    private:
        // FlyingBird.isLandableBlock: leaves, or a sturdy top face.
        bool IsLandableBlock(int x, int y, int z) const;

        bool m_landed = true;          // FlyingBird ctor: setIsBirdLanded(true)
        bool m_hasTarget = false;
        glm::ivec3 m_targetPosition{0};
        int  m_currentFlightTime = 0;
    };

    // TF passive/TinyBird. MAX_HEALTH 4, MOVEMENT_SPEED 0.2, STEP_HEIGHT 1.0.
    // Spooking is TF's own isSpooked: hurt, or a player within 4 blocks
    // holding seeds (#c:seeds, the tempt tag).
    //
    // TinyBirdVariant (the twilight/tiny_bird_variant registry: blue, brown,
    // gold, red, in bootstrap order — none names biomes, so finalizeSpawn's
    // getVariant is a uniform pick of the four). The variant rides the wire
    // variant byte in that order and saves as the mod's "variant" key; the
    // synced default before a spawn roll is RED.
    class TinyBird : public FlyingBird {
    public:
        enum Variant : uint8_t { Blue = 0, Brown = 1, Gold = 2, Red = 3, VariantCount = 4 };

        explicit TinyBird(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        uint8_t GetVariantByte() const override { return m_variant; }
        void    SetVariantByte(uint8_t v) override { m_variant = v < VariantCount ? v : Red; }

        // TinyBird.finalizeSpawn: TinyBirdVariant.getVariant.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        // The variant's registry name ("blue", ...) and back; Red on a miss.
        static const char* VariantName(uint8_t v);
        static uint8_t     VariantFromName(std::string_view name);

    protected:
        void RegisterGoals() override;
        bool IsSpooked() override;

    private:
        uint8_t m_variant = Red;
    };

    // TF passive/Raven extends FlyingBird. MAX_HEALTH 10, MOVEMENT_SPEED 0.2,
    // STEP_HEIGHT 1.0. Spooked only when hurt; tempted by #c:seeds.
    class Raven : public FlyingBird {
    public:
        explicit Raven(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
        bool IsSpooked() override;
    };

    // TF passive/Penguin extends Bird. MAX_HEALTH 10, MOVEMENT_SPEED 0.2.
    // Breeds on #minecraft:fishes (TFItemTags.PENGUIN_TEMPT_ITEMS).
    class Penguin : public TFBird {
    public:
        explicit Penguin(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

        // Penguin.canSpawn (the registered predicate): the block below is in
        // #minecraft:ice (TFBlockTags.PENGUINS_SPAWNABLE_ON). Penguin
        // .checkSpawnRules returns true, so this is the whole rule.
        static bool CheckPenguinSpawnRules(EntityLevel& level, const glm::ivec3& pos);
        // Penguin.checkSpawnRules: always true (PathfinderMob's walk-target
        // test is waived — ice scores low).
        bool CheckSpawnRules(EntityLevel& level, SpawnReason reason) override {
            (void)level; (void)reason;
            return true;
        }

    protected:
        void RegisterGoals() override;
    };

    // ── Kobold ─────────────────────────────────────────────────────────────

    // TF monster/Kobold. MAX_HEALTH 13, MOVEMENT_SPEED 0.28, ATTACK_DAMAGE 4.
    // TF's PanicOnFlockDeathGoal (bolt when a kobold within 4 blocks is
    // dying) is rebuilt as MC's PanicGoal at the same priority and speed,
    // which bolts when THIS kobold is hurt — the port brief's "panics when
    // hurt". The bread half (SeekBreadGoal, RunAwayWhileHoldingBreadGoal,
    // loot pickup, munching) and FlockToSameKindGoal have no engine
    // counterpart and are left out.
    class Kobold : public Monster {
    public:
        explicit Kobold(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

    // ── Redcap ─────────────────────────────────────────────────────────────

    // TF monster/Redcap. MAX_HEALTH 20, MOVEMENT_SPEED 0.28. TF equips an
    // iron pickaxe (mainhand) and iron boots in populateDefaultEquipmentSlots;
    // this engine has no mob equipment, so the pickaxe's +3 attack damage is
    // folded into ATTACK_DAMAGE (Monster's 2 + 3 = 5) and the boots' armour
    // is not modelled. The TNT behaviour (AvoidAnyEntityGoal on primed TNT,
    // RedcapShyGoal, RedcapLightTNTGoal) is left out.
    class Redcap : public Monster {
    public:
        explicit Redcap(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

    protected:
        void RegisterGoals() override;
    };

} // namespace Game
