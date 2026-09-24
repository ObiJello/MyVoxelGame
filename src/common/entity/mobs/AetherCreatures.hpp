// File: src/common/entity/mobs/AetherCreatures.hpp
//
// The Aether's pass-two creatures (docs/mod-ports.md): the rest of its biome
// spawners and its dungeon mobs, ported from the mod's own classes
// (mods_reference/aether, entity/passive, entity/monster, entity/monster/
// dungeon). Shared Aether machinery (saddles, the winged-bird animation,
// poison needles, the id lookups) lives in AetherMobs.hpp; MakeAetherMob
// there builds every type here.
//
//   Moa          — MountableAnimal + WingedBird: three MoaTypes (blue /
//                  white / black, the mod's weighted 100/50/25 roll), glides,
//                  lays its type's egg, hunts swets and aechor plants once
//                  player-grown.
//   Aerbunny     — AetherAnimal: slow-falling, double-jumps off ledges with a
//                  puff, runs from players that hit it.
//   Aerwhale     — FlyingMob: sets a course 32-48 blocks away and cruises to
//                  it, pitching and turning slowly (AETHER_AERWHALE).
//   Swet         — Slime-shaped jumper (blue / golden): hunts players and
//                  "consumes" them — three hops carrying the prey, then a
//                  fling (see the class).
//   Whirlwind    — Mob drawing only particles: wanders, lifts and spins every
//                  entity near it, drops junk; the evil one stops on a player
//                  and its junk can be a creeper.
//   AechorPlant  — stationary PathfinderMob that fires poison needles.
//   Mimic        — a chest that bites.
//   Sentry       — a Slime of carved stone that wakes near players, hops at
//                  them and explodes on contact.
//   Valkyrie     — flying swordswoman: lunges down at targets, hovers.
//   FireMinion   — the Sun Spirit's melee minion.
//
// Numbers from each class's createMobAttributes, registerGoals and tick;
// mod goals with no engine counterpart are small goals here, named after the
// mod's. Sounds and the Aether's own particle types wait on those systems.
#pragma once

#include "common/entity/Animal.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/RangedAttackMob.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/mobs/AetherMobs.hpp"
#include "common/entity/mobs/Slime.hpp"

#include <cstdint>
#include <memory>

namespace Game {

    // ── Moa ────────────────────────────────────────────────────────────────

    // Aether passive/Moa. MAX_HEALTH 35, FOLLOW_RANGE 16, ATTACK_DAMAGE 5;
    // MOVEMENT_SPEED is the attribute (1.0) times MoaType.speed (0.155 for
    // all three), which is what Moa.getSpeed hands the move control — the
    // engine reads the attribute directly, so it holds the product.
    //
    // The MoaType rides the wire as the variant byte (0 blue, 1 white,
    // 2 black — AetherMoaTypes' weighted roll, spawn_chance 100/50/25) and is
    // saved as the mod's "MoaType" id. The anim byte adds bit 2 sitting,
    // bit 3 hungry, bit 4 player-grown to the mountable bits.
    //
    // Not ported: the Nature Staff (sitting and MoaFollowGoal toggle on it —
    // no such item yet; the saved flags round-trip), feeding a hungry baby
    // (MOA_FOOD_ITEMS = aechor petal) and the incubator that makes moas
    // player-grown, the moa skins perk, player riding (see AetherMobs.hpp).
    // A wild moa is therefore never saddleable, exactly as in the mod.
    class Moa : public MountableAetherAnimal {
    public:
        explicit Moa(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        enum MoaTypeId : uint8_t { kBlue = 0, kWhite = 1, kBlack = 2, kMoaTypeCount = 3 };

        uint8_t GetMoaType() const { return m_moaType; }
        void    SetMoaType(uint8_t t) { m_moaType = t < kMoaTypeCount ? t : kBlue; }
        // MoaType.maxJumps (moa_type/*.json): 3 / 4 / 8.
        int  GetMaxJumps() const;

        bool IsSitting() const { return m_sitting; }
        bool IsHungry() const { return m_hungry; }
        bool IsPlayerGrown() const { return m_playerGrown; }

        bool IsFood(uint32_t) const override { return false; }
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }
        bool IsSaddleable() const override {
            return MountableAetherAnimal::IsSaddleable() && m_playerGrown;
        }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void Tick() override;
        void AiStep() override;
        int  GetMaxFallDistance() const override;

        // Moa.getDefaultDimensions: height halved while sitting and again as
        // a baby (width unchanged).
        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;

        uint8_t GetVariantByte() const override { return m_moaType; }
        void    SetVariantByte(uint8_t v) override { SetMoaType(v); }
        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

        float GetWingsAnimation(float partialTick) const { return m_wings.Get(partialTick); }

        // AetherMoaTypes.getWeightedChance.
        static uint8_t RandomMoaType(JavaRandom& rng);

    protected:
        void RegisterGoals() override;

    private:
        float DimensionFactor() const;

        uint8_t        m_moaType = kBlue;
        bool           m_sitting = false;
        bool           m_hungry = false;
        bool           m_playerGrown = false;
        int            m_amountFed = 0;
        int            m_remainingJumps = 0;
        int            m_eggTime = 0;
        WingedBirdAnim m_wings;
    };

    // Moa's two target goals: NearestAttackableTargetGoal over swets / the
    // aechor plant, gated on `getFollowing() == null && isPlayerGrown() &&
    // !isBaby()` (the following half never holds here — no Nature Staff).
    class MoaHuntTargetGoal : public NearestAttackableTargetGoal {
    public:
        MoaHuntTargetGoal(Moa* moa, const EntityTypeId* types, int typeCount)
            : NearestAttackableTargetGoal(moa, types, typeCount, /*mustSee=*/false),
              m_moa(moa) {}
        bool CanUse() override {
            return m_moa->IsPlayerGrown() && !m_moa->IsBaby() &&
                   NearestAttackableTargetGoal::CanUse();
        }
    private:
        Moa* m_moa;
    };

    // ── Aerbunny ───────────────────────────────────────────────────────────

    // Aether passive/Aerbunny. MAX_HEALTH 6, MOVEMENT_SPEED 0.28. Falls at
    // most 0.1 blocks/tick unless thrown (fast-falling until it lands);
    // AerbunnyMoveControl hops it off the ground and, over a two-block gap,
    // double-jumps mid-air with a puff (puffiness 11, counting down 1/tick —
    // the anim byte). Hit by a player it is afraid for 100-149 ticks
    // (RunWhenAfraid). Not ported: riding on a player's head (the mod's
    // signature trick) — players cannot carry passengers here.
    class Aerbunny : public Animal {
    public:
        explicit Aerbunny(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsFood(uint32_t itemId) const override;   // AERBUNNY_TEMPTATION_ITEMS: blue berry
        std::unique_ptr<Animal> CreateBaby() override;
        float GetWalkTargetValue(const glm::ivec3& pos) const override;

        void Tick() override;
        void AiStep() override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void HandleEntityEvent(uint8_t id) override;

        // Aerbunny.midairJump — called by the move control.
        void MidairJump();

        int  GetAfraidTime() const { return m_afraidTime; }
        int  GetPuffiness() const { return m_puffiness; }
        int  GetPuffSubtract() const { return m_puffSubtract; }

        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_puffiness); }
        void    SetAnimStateByte(uint8_t v) override { m_puffiness = v; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        int  m_puffiness = 0;
        int  m_puffSubtract = 0;
        int  m_afraidTime = 0;
        bool m_fastFalling = false;
    };

    // Aerbunny.AerbunnyMoveControl.
    class AerbunnyMoveControl : public MoveControl {
    public:
        explicit AerbunnyMoveControl(Aerbunny* bunny) : MoveControl(bunny), m_bunny(bunny) {}
        void Tick() override;
    private:
        bool CheckForSurfaces(int x, int y, int z) const;
        Aerbunny* m_bunny;
    };

    // Aerbunny.RunWhenAfraid.
    class AerbunnyRunWhenAfraidGoal : public Goal {
    public:
        AerbunnyRunWhenAfraidGoal(Aerbunny* bunny, double speed);
        bool CanUse() override { return m_bunny->GetAfraidTime() > 0; }
        bool CanContinueToUse() override;
        void Start() override;
        const char* Name() const override { return "AerbunnyRunWhenAfraidGoal"; }
    private:
        Aerbunny* m_bunny;
        double    m_speed;
    };

    // ── Aerwhale ───────────────────────────────────────────────────────────

    // Aether passive/Aerwhale extends FlyingMob. MAX_HEALTH 20, FLYING_SPEED
    // 0.2, STEP_HEIGHT 0.4; fire-immune, no fall damage, 1-3 xp, clusters of
    // one. The look control is blank; AerwhaleMoveControl steers pitch and
    // yaw toward the course SetTravelCourseGoal picks and sets the velocity
    // outright. The mod syncs xRot/yRot through its own data accessors; the
    // engine's rotation sync carries them. The SerenityLowes easter egg is
    // not ported.
    class Aerwhale : public Mob {
    public:
        explicit Aerwhale(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool FireImmune() const override { return true; }
        bool CauseFallDamage(double, float) override { return false; }
        void Travel(const glm::dvec3& input) override;
        int  GetXpReward() const override;
        int  GetMaxSpawnClusterSize() const override { return 1; }

    protected:
        void RegisterGoals() override;
    };

    class AerwhaleMoveControl : public MoveControl {
    public:
        explicit AerwhaleMoveControl(Aerwhale* whale) : MoveControl(whale) {}
        void Tick() override;
        using MoveControl::SetWait;
    private:
        bool IsColliding(const glm::dvec3& dir) const;
    };

    // Aerwhale.SetTravelCourseGoal.
    class AerwhaleTravelCourseGoal : public Goal {
    public:
        explicit AerwhaleTravelCourseGoal(Mob* mob);
        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        void Start() override;
        const char* Name() const override { return "AerwhaleTravelCourseGoal"; }
    private:
        Mob* m_mob;
    };

    // ── Swet ───────────────────────────────────────────────────────────────

    // Aether monster/Swet extends Slime (blue and golden share the class).
    // MAX_HEALTH 12, MOVEMENT_SPEED 0.4, FOLLOW_RANGE 14, KNOCKBACK_RESISTANCE
    // 0.5, xpReward 5; jump power 0.325; size 1 (the Slime box is 0.9 x 0.9
    // scaled by 1 - waterDamageScale). Water dissolves it: +0.02 scale per
    // wet tick, gone at 0.9.
    //
    // CONSUMING: the mod seats its prey as a passenger and hops three times
    // (+0.65, +0.75, +1.55) before dissolving and dropping it from the top of
    // the third. A player cannot be a passenger here, so the prey is held by
    // reference instead: each hop hands the prey the same lift (plus a pull
    // toward the swet) through AddDeltaMovement, and the third hop releases
    // it with the fling — the same arc, the fall damage doing the harm as in
    // the mod. The anim byte carries bit 0 mid-jump and bits 1-6 the water
    // damage scale in 0.02 steps. Not ported: the swet cape / banner pacifying
    // (no such items), a friendly swet as a mount.
    class Swet : public Mob {
    public:
        Swet(EntityTypeId type, EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        float BaseBbWidth()   const override;
        float BaseBbHeight()  const override;
        float BaseEyeHeight() const override;
        float GetJumpPower() const override { return 0.325f; }
        void  JumpFromGround() override;

        void Tick() override;
        void AiStep() override;
        void ClearReferenceTo(const Entity* entity) override;

        // Swet.getJumpDelay: 10 + rand(20).
        int GetJumpDelay();
        bool HasPrey() const { return m_prey != nullptr; }
        LivingEntity* GetPrey() const { return m_prey; }
        void Consume(LivingEntity& prey);
        void ReleasePrey();
        bool GetMidJump() const { return m_midJump; }
        bool WasOnGround() const { return m_wasOnGround; }
        int  GetJumpTimer() const { return m_jumpTimer; }
        float GetWaterDamageScale() const { return m_waterDamageScale; }

        // Swet.swetHeight / swetWidth, client-side.
        float GetSwetHeight(float partialTick) const {
            return m_swetHeightO + (m_swetHeight - m_swetHeightO) * partialTick;
        }
        float GetSwetWidth(float partialTick) const {
            return m_swetWidthO + (m_swetWidth - m_swetWidthO) * partialTick;
        }

        uint8_t GetAnimStateByte() const override;
        void    SetAnimStateByte(uint8_t v) override;

        // blue_swet: one swet ball (the table) + one blue aercloud (a block
        // item — this hook); golden_swet: one glowstone (block item).
        void DropCustomDeathLoot(EntityLevel& level) override;

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        LivingEntity* m_prey = nullptr;
        float m_waterDamageScale = 0.0f;
        bool  m_midJump = false;
        bool  m_wasOnGround = false;
        int   m_ascendTimer = 0;
        int   m_jumpTimer = 0;
        float m_swetHeight = 1.0f, m_swetHeightO = 1.0f;
        float m_swetWidth = 1.0f, m_swetWidthO = 1.0f;
    };

    // Swet.SwetMoveControl — the Slime move control with the aggressive
    // jump delay divided by 6 (Slime's is 3), idle while ridden.
    class SwetMoveControl : public MoveControl {
    public:
        explicit SwetMoveControl(Swet* swet);
        void SetDirection(float yRot, bool aggressive) { m_yRot = yRot; m_aggressive = aggressive; }
        void SetWantedMovement(double speed) { m_speedModifier = speed; m_operation = Operation::MoveTo; }
        void SetCanJump(bool canJump) { m_canJump = canJump; }
        bool CanJump() const { return m_canJump; }
        float GetYRot() const { return m_yRot; }
        void Tick() override;
    private:
        Swet* m_swet;
        float m_yRot = 0.0f;
        int   m_jumpDelay = 0;
        bool  m_aggressive = false;
        bool  m_canJump = false;
    };

    // Swet.ConsumeGoal / HuntGoal / SwetRandomDirectionGoal /
    // SwetKeepOnJumpingGoal.
    class SwetConsumeGoal : public Goal {
    public:
        explicit SwetConsumeGoal(Swet* swet);
        bool CanUse() override { return m_swet->HasPrey(); }
        void Stop() override { m_jumps = 0; }
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "SwetConsumeGoal"; }
    private:
        void MoveHorizontal(float forward, float rotation);
        Swet* m_swet;
        int   m_jumps = 0;
        float m_chosenDegrees = 0.0f;
    };

    class SwetHuntGoal : public Goal {
    public:
        explicit SwetHuntGoal(Swet* swet);
        bool CanUse() override;
        bool CanContinueToUse() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "SwetHuntGoal"; }
    private:
        Swet* m_swet;
    };

    class SwetRandomDirectionGoal : public Goal {
    public:
        explicit SwetRandomDirectionGoal(Swet* swet);
        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "SwetRandomDirectionGoal"; }
    private:
        Swet* m_swet;
        float m_chosenDegrees = 0.0f;
        int   m_nextRandomizeTime = 0;
    };

    class SwetKeepOnJumpingGoal : public Goal {
    public:
        explicit SwetKeepOnJumpingGoal(Swet* swet);
        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "SwetKeepOnJumpingGoal"; }
    private:
        Swet* m_swet;
    };

    // ── Whirlwinds ─────────────────────────────────────────────────────────

    // Aether monster/AbstractWhirlwind + PassiveWhirlwind / EvilWhirlwind.
    // MAX_HEALTH 10, MOVEMENT_SPEED 0.025, FOLLOW_RANGE 16; invulnerable (hurt
    // always fails), fire-immune, never rides, clusters of one. Lives
    // 512-1023 ticks (evil: half), less while inside blocks; dies stuck
    // against a ceiling for 40 ticks. Every entity within the box stretched
    // 2.5 blocks toward +x/+y/+z (except aechor plants and whirlwinds) is
    // lifted and swung round it; a player gets the same motion as a pushed
    // velocity (AddDeltaMovement against its known movement). Every 128
    // ticks with a target, a 1-in-4 roll drops from the junk table
    // (selectors/whirlwind_junk; the evil table's 60-weight entry spawns a
    // creeper). Item entities are not entities here and are not lifted.
    //
    // The mod rolls the movement angle and curve in the constructor ON THE
    // CLIENT ONLY, so a server-side whirlwind always starts at angle 0 with
    // no curve, turning round only at a drop — ported as is. The particles
    // are the Aether's own whirlwind types; this draws vanilla POOF (white,
    // the passive one) and LARGE_SMOKE (the evil one) on the same path.
    class Whirlwind : public Mob {
    public:
        Whirlwind(EntityTypeId type, EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool IsEvil() const { return GetType() == EntityTypeId::EvilWhirlwind; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void Tick() override;
        void AiStep() override;
        bool Hurt(MobDamageSource, float, Entity*) override { return false; }
        bool FireImmune() const override { return true; }
        bool CanRide(const Entity&) const override { return false; }
        int  GetMaxSpawnClusterSize() const override { return 1; }

        float GetMovementAngle() const { return m_movementAngle; }
        float GetMovementCurve() const { return m_movementCurve; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        void SpawnDrops();
        void SpawnParticles();

        int   m_lifeLeft = 0;
        int   m_dropsTimer = 0;
        int   m_stuckTick = 0;
        float m_movementAngle = 0.0f;
        float m_movementCurve = 0.0f;
        int   m_color = 0;
    };

    // AbstractWhirlwind.MoveGoal.
    class WhirlwindMoveGoal : public Goal {
    public:
        explicit WhirlwindMoveGoal(Whirlwind* whirlwind);
        bool CanUse() override { return true; }
        void Tick() override;
        const char* Name() const override { return "WhirlwindMoveGoal"; }
    private:
        Whirlwind* m_whirlwind;
        float m_movementAngle = 0.0f;
        float m_movementCurve = 0.0f;
    };

    // ── Aechor plant ───────────────────────────────────────────────────────

    // Aether monster/AechorPlant extends PathfinderMob. MAX_HEALTH 15,
    // MOVEMENT_SPEED 0, KNOCKBACK_RESISTANCE 1, xpReward 5. Size 1-4 (the
    // variant byte) sets the box: 0.75 + size/8 wide, 0.5 + size*0.075 tall.
    // RangedAttackGoal(1.0, 60, 10) fires a lobbed poison needle; dies when
    // the block under it stops being aether grass. Its hasLineOfSight adds
    // an 8-block cap the engine's Sensing cannot take (the 10-block goal
    // radius still bounds it). Not ported: the skyroot-bucket poison harvest
    // (Poison Remaining round-trips), the flower deterrent in its spawn rule.
    class AechorPlant : public PathfinderMob, public RangedAttackMob {
    public:
        explicit AechorPlant(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        int  GetSize() const { return m_size; }
        void SetSize(int size) { m_size = size; }
        bool IsTargetingEntity() const { return m_targetingEntity; }
        float GetSinage(float partialTick) const { return m_sinage + m_sinageAdd * partialTick; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void PerformRangedAttack(LivingEntity& target, float power) override;

        void Tick() override;
        void AiStep() override;
        void JumpFromGround() override {}
        bool IsPushable() const override { return false; }

        float BaseBbWidth()   const override { return 0.75f + static_cast<float>(m_size) * 0.125f; }
        float BaseBbHeight()  const override { return 0.5f + static_cast<float>(m_size) * 0.075f; }
        float BaseEyeHeight() const override { return BaseBbHeight() / 1.15f; }

        uint8_t GetVariantByte() const override { return static_cast<uint8_t>(m_size); }
        void    SetVariantByte(uint8_t v) override { m_size = v; }
        uint8_t GetAnimStateByte() const override { return m_targetingEntity ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_targetingEntity = (v & 1) != 0; }

        void SaveModNbt(ModNbtOut& out) const override;
        void LoadModNbt(const ModNbtIn& in) override;

    protected:
        void RegisterGoals() override;

    private:
        int   m_size = 0;
        int   m_poisonRemaining = 2;
        bool  m_targetingEntity = false;
        float m_sinage = 0.0f;
        float m_sinageAdd = 0.0f;
    };

    // ── Dungeon mobs ───────────────────────────────────────────────────────

    // Aether monster/dungeon/Mimic. MAX_HEALTH 40, ATTACK_DAMAGE 3,
    // MOVEMENT_SPEED 0.28, FOLLOW_RANGE 8. Ignores damage from other mimics;
    // anything else that hits it becomes its target. The chest and the junk
    // pool (entities/mimic: 1-3 rolls over darts, tools, ores) ride
    // DropCustomDeathLoot — block items and Aether items resolved by slug,
    // falling back to the nearest vanilla item.
    class Mimic : public Monster {
    public:
        explicit Mimic(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;
        void DropCustomDeathLoot(EntityLevel& level) override;

    protected:
        void RegisterGoals() override;
    };

    // Aether monster/dungeon/Sentry extends Slime: fixed size 1 (setSize is a
    // no-op), MAX_HEALTH 10, MOVEMENT_SPEED 0.6, ATTACK_DAMAGE 2. Wakes after
    // 24 ticks with a player inside 8 blocks (sleeps again when none is);
    // only an awake sentry jumps and runs the slime goals. Touching a living
    // thing while awake (and 20 ticks old) hurts it 1, knocks it (0.3, 0.4,
    // 0.3) and detonates a radius-1 MOB explosion, removing the sentry. The
    // awake flag is anim byte bit 0 (sentry_lit + the eye layer). xpReward
    // stays 0 — Slime only sets it in setSize.
    class Sentry : public Slime {
    public:
        explicit Sentry(EntityLevel* level);

        void SetSize(int, bool) override {}
        int  GetXpReward() const override { return 0; }
        float BaseBbWidth()   const override { return 0.9f; }
        float BaseBbHeight()  const override { return 0.9f; }
        float BaseEyeHeight() const override { return 0.765f; }

        void Tick() override;
        void JumpFromGround() override;

        bool IsAwake() const { return m_awake; }
        uint8_t GetAnimStateByte() const override { return m_awake ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_awake = (v & 1) != 0; }

        // entities/sentry: one of carved stone / sentry stone (block items).
        void DropCustomDeathLoot(EntityLevel& level) override;

    private:
        void ExplodeAt(LivingEntity& entity);

        bool  m_awake = false;
        float m_timeSpotted = 0.0f;
    };

    // Aether monster/dungeon/AbstractValkyrie + Valkyrie (NeutralMob).
    // MOVEMENT_SPEED 0.5, KNOCKBACK_RESISTANCE 0.25, FOLLOW_RANGE 16,
    // ATTACK_DAMAGE 10, MAX_HEALTH 50; not despawned in peaceful. Hovers
    // (a 0.0225 lift when the fall is settling) and lunges down at its target
    // from the air (LungeGoal). "Entity on ground" is anim byte bit 0 (the
    // wing flap). Its MostDamageTargetGoal (aggro by damage dealt) is the
    // engine's HurtByTargetGoal; the NeutralMob anger target and the dialog
    // lines are not ported. ValkyrieTeleportGoal only teleports onto the
    // Silver Dungeon's locked angelic stone, looked up by slug.
    class Valkyrie : public Monster {
    public:
        explicit Valkyrie(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        void Tick() override;
        void Travel(const glm::dvec3& input) override;
        void JumpFromGround() override;
        void HandleEntityEvent(uint8_t id) override;

        bool IsEntityOnGround() const { return m_entityOnGround; }
        void SetEntityOnGround(bool v) { m_entityOnGround = v; }
        double GetLastMotionY() const { return m_lastMotionY; }
        int  GetLungeCooldown() const { return m_lungeCooldown; }
        void SetLungeCooldown(int c) { m_lungeCooldown = c; }

        // AbstractValkyrie.teleportAroundTarget.
        bool TeleportAroundTarget(Entity& target);

        uint8_t GetAnimStateByte() const override { return m_entityOnGround ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_entityOnGround = (v & 1) != 0; }

        // entities/valkyrie: one victory medal (resolved by slug; gold
        // nugget until the item exists).
        void DropCustomDeathLoot(EntityLevel& level) override;

    protected:
        void RegisterGoals() override;

    private:
        bool   m_entityOnGround = true;
        int    m_lungeCooldown = 0;
        double m_lastMotionY = 0.0;
    };

    // AbstractValkyrie.ValkyrieMoveControl: a JUMPING operation becomes
    // MOVE_TO before the base tick.
    class ValkyrieMoveControl : public MoveControl {
    public:
        explicit ValkyrieMoveControl(Mob* mob) : MoveControl(mob) {}
        void Tick() override;
    };

    class ValkyrieLungeGoal : public Goal {
    public:
        ValkyrieLungeGoal(Valkyrie* valkyrie, double speed, int cooldownMax);
        bool CanUse() override;
        void Tick() override;
        void Stop() override { m_valkyrie->SetLungeCooldown(m_cooldownMax); }
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "ValkyrieLungeGoal"; }
    private:
        Valkyrie* m_valkyrie;
        double    m_speed;
        int       m_cooldownMax;
        int       m_flyingTicks = 0;
    };

    class ValkyrieTeleportGoal : public Goal {
    public:
        explicit ValkyrieTeleportGoal(Valkyrie* valkyrie);
        bool CanUse() override { return true; }
        void Tick() override;
        const char* Name() const override { return "ValkyrieTeleportGoal"; }
    private:
        Valkyrie* m_valkyrie;
        int       m_teleportTimer = -1;
    };

    // Aether monster/dungeon/FireMinion. FOLLOW_RANGE 40, MOVEMENT_SPEED
    // 0.25, ATTACK_DAMAGE 15, MAX_HEALTH 40; fire-immune; a snowball hits it
    // for +3. The client trails a falling flame each tick — drawn as SMOKE
    // until the engine has a FLAME particle. The frozen-minion name easter
    // egg and the Sun Spirit's immunity are not ported.
    class FireMinion : public Monster {
    public:
        explicit FireMinion(EntityLevel* level);

        static void CreateAttributes(AttributeMap& out);

        bool FireImmune() const override { return true; }
        void Tick() override;
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

    protected:
        void RegisterGoals() override;
    };

} // namespace Game
