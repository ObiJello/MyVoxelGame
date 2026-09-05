// File: src/common/entity/mobs/Fish.hpp
//
// MC AbstractFish + AbstractSchoolingFish — cod, salmon, tropical fish,
// pufferfish.
//
// A fish is a WaterBound pathfinder with the FishMoveControl and one habit:
// out of water on solid ground it flops (a 0.4-up hop with random sideways
// jerks) until it dies or lands back in water. Schooling adds the leader
// system: one fish accumulates followers up to the school size (8), followers
// give up their own wandering and path to the leader — which is why a school
// moves as one body with a single decision-maker.
#pragma once

#include "common/entity/Mob.hpp"
#include "common/entity/mobs/GenericMobs.hpp"

namespace Game {

    // MC WaterAnimal.handleAirSupply — the INVERSE of LivingEntity's
    // drowning: a water animal out of water loses 1 air per tick and takes
    // 2.0 drown damage every time air hits -20; in water (or dead) its air
    // pins at 300. Called from BaseTick with the PRE-super air value, because
    // LivingEntity's own air block would otherwise refill a beached fish
    // (+4/tick out of water) faster than this drains it. Shared by Fish,
    // Squid and Tadpole — MC's WaterAnimal subclasses here.
    void HandleWaterAnimalAirSupply(class Mob& mob, int preTickAirSupply);

    class Fish : public PathfinderMob {
    public:
        Fish(EntityTypeId type, EntityLevel* level);

        // MC AbstractFish.getMaxSpawnClusterSize.
        int GetMaxSpawnClusterSize() const override { return 8; }

        // MC removeWhenFarAway: !fromBucket && !hasCustomName — neither
        // exists yet, so a wild fish always despawns like any water ambient.
        bool RemoveWhenFarAway(double) const override { return true; }

        // MC WaterAnimal.getBaseExperienceReward (WaterAnimal.java:32-34):
        // 1..3, same roll as Animal's. Out-of-line: needs the level random.
        int GetXpReward() const override;

        // MC AbstractFish.aiStep — the beached flop.
        void AiStep() override;

        // MC WaterAnimal.baseTick: capture air, super, handleAirSupply —
        // a beached fish now actually suffocates like MC's.
        void BaseTick() override;

        // MC canRandomSwim — a schooling follower stops wandering on its own.
        virtual bool CanRandomSwim() const { return true; }

    protected:
        void RegisterGoals() override;
    };

    class SchoolingFish : public Fish {
    public:
        SchoolingFish(EntityTypeId type, EntityLevel* level);

        // MC: the spawn cluster IS the school.
        int GetMaxSpawnClusterSize() const override { return GetMaxSchoolSize(); }
        virtual int GetMaxSchoolSize() const { return Fish::GetMaxSpawnClusterSize(); }

        bool IsFollower() const { return m_leader && m_leader->IsAlive(); }
        bool HasFollowers() const { return m_schoolSize > 1; }
        bool CanBeFollowed() const {
            return HasFollowers() && m_schoolSize < GetMaxSchoolSize();
        }
        bool InRangeOfLeader() const {
            return m_leader && DistanceToSqr(*m_leader) <= 121.0;
        }

        void StartFollowing(SchoolingFish* leader) {
            m_leader = leader;
            ++leader->m_schoolSize;
        }
        void StopFollowing() {
            if (m_leader) --m_leader->m_schoolSize;
            m_leader = nullptr;
        }
        void PathToLeader();

        bool CanRandomSwim() const override { return !IsFollower(); }

        void Tick() override;

        // MC SchoolSpawnGroupData: the first fish of a pack becomes leader,
        // the rest of the pack spawns already following it.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        void ClearReferenceTo(const Entity* entity) override {
            Fish::ClearReferenceTo(entity);
            if (m_leader == entity) m_leader = nullptr;
        }

    protected:
        void RegisterGoals() override;

    private:
        SchoolingFish* m_leader = nullptr;
        int m_schoolSize = 1;
    };

    // MC animal/fish/TropicalFish — only the piece spawning reads. In MC,
    // finalizeSpawn's 10% rare-variant roll (TropicalFish.java:198-207) also
    // clears `isSchool`, and isMaxGroupSizeReached returns !isSchool
    // (TropicalFish.java:97-99) — so one tropical fish in ten spawns as a
    // loner and caps its spawn group at itself. The variants themselves are
    // not modelled (the port draws one tropical fish texture); the roll keeps
    // only its spawning consequence. Server-side only: the client's display
    // mirror stays a plain SchoolingFish.
    class TropicalFish : public SchoolingFish {
    public:
        explicit TropicalFish(EntityLevel* level)
            : SchoolingFish(EntityTypeId::TropicalFish, level) {}

        // MC TropicalFish.isMaxGroupSizeReached (TropicalFish.java:97-99).
        bool IsMaxGroupSizeReached(int) const override { return !m_isSchool; }

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

    private:
        bool m_isSchool = true;   // MC TropicalFish.isSchool
    };

    // MC Pufferfish — a non-schooling fish with the three-stage puff-up:
    // PufferfishPuffGoal starts inflating when a scary LivingEntity comes
    // within 2 blocks; tick() walks the SMALL → MID → FULL state machine and
    // back down; while puffed, anything touching it takes (1 + state) damage
    // and POISON for 60 * state ticks.
    //
    // The puff state rides the wire's anim byte (MC syncs it as PUFF_STATE)
    // so the client's copy has it; the model-scale change per state
    // (0.5 / 0.7 / 1.0, refreshDimensions) is render/dimension work the
    // entity-type table does not carry yet. MC's nausea on player sting is a
    // skipped-comment at the site (no NAUSEA gameplay half exists — it is a
    // pure screen effect).
    class Pufferfish : public Fish {
    public:
        explicit Pufferfish(EntityLevel* level);

        // MC Pufferfish.STATE_*.
        static constexpr int kStateSmall = 0;
        static constexpr int kStateMid   = 1;
        static constexpr int kStateFull  = 2;

        int  GetPuffState() const { return m_puffState; }
        // MC's read applies min(saved, 2); the clamp lives here so every
        // caller gets it.
        void SetPuffState(int state) { m_puffState = state < 0 ? 0 : (state > 2 ? 2 : state); }

        // MC's SCARY_MOB selector: not a creative player, and not in the
        // NOT_SCARY_FOR_PUFFERFISH tag (the aquatic neighbours).
        static bool IsScaryTarget(const LivingEntity& target);

        // Called by PufferfishPuffGoal (MC's inner class writes the counters
        // directly).
        void StartPuffing() { m_inflateCounter = 1; m_deflateTimer = 0; }
        void StopPuffing()  { m_inflateCounter = 0; }

        // MC Pufferfish.tick — the inflate/deflate state machine (server).
        void Tick() override;

        // MC Pufferfish.aiStep — the touch sweep while puffed.
        void AiStep() override;

        uint8_t GetAnimStateByte() const override {
            return static_cast<uint8_t>(m_puffState);
        }
        void SetAnimStateByte(uint8_t v) override {
            m_puffState = v <= 2 ? static_cast<int>(v) : 2;
        }

    protected:
        void RegisterGoals() override;

    private:
        // MC Pufferfish.touch — damage + poison one victim.
        void Touch(LivingEntity& mob);

        int m_puffState = 0;        // MC PUFF_STATE (synced)
        int m_inflateCounter = 0;   // MC inflateCounter
        int m_deflateTimer = 0;     // MC deflateTimer
    };

    // MC animal/dolphin/Dolphin. MAX_HEALTH 10, MOVEMENT_SPEED 1.2 (a swim
    // speed — SmoothSwimmingMoveControl's 0.1 land factor is what keeps a
    // beached dolphin from sprinting), ATTACK_DAMAGE 3.
    //
    // The parts that make a dolphin a dolphin here: the smooth-swim controls
    // (SmoothSwimmingMoveControl(85, 10, 0.02, 0.1, gravity) +
    // SmoothSwimmingLookControl(10)), the breach (DolphinJumpGoal), the
    // beached flop + dry-out damage from tick(), melee retaliation
    // (guardian-grudge HurtByTarget + MeleeAttackGoal(1.2)), fleeing
    // guardians, TryFindWaterGoal, and the air supply — 4800 ticks of
    // breath, drowning underwater, BreathAirGoal surfacing it below 140.
    // Not modelled, each named at its site (DolphinGoals.hpp): treasure
    // hunting (no structures; the fed-fish flag that arms it also rides
    // mobInteract feeding, skipped with items), swim-with-player (no swim
    // pose / DOLPHINS_GRACE), item play, boat following.
    //
    // MC's AgeableWaterCreature half IS modelled: a dolphin is an AgeableMob
    // (age, baby box, the 26.x baby mesh), 10% of a spawn pack past the first
    // member spawns as a calf (Dolphin.finalizeSpawn), and a spawn egg on an
    // adult makes one (getBreedOffspring). Dolphins still do not breed.
    class Dolphin : public AgeableMob {
    public:
        explicit Dolphin(EntityLevel* level);

        // MC Dolphin.finalizeSpawn: full air, level pitch, then the
        // AgeableMobGroupData(0.1F) pack roll.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        static void CreateAttributes(AttributeMap& out);

        // MC WaterCreature despawns like any water ambient.
        bool RemoveWhenFarAway(double) const override { return true; }

        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1..3. Out-of-line: needs the
        // level random.
        int GetXpReward() const override;

        // MC Dolphin.canAttack: never as a baby.
        bool CanAttack(const LivingEntity& target) const override {
            return !IsBaby() && AgeableMob::CanAttack(target);
        }

        // ── Air (MC Dolphin's 4800-tick lungs) ────────────────────────────
        // A dolphin DROWNS underwater — it is not in the can-breathe tag, so
        // LivingEntity's air block ticks it down while its eyes are
        // submerged — but a single breath at the surface refills everything.
        // WaterAnimal's beached air-loss is overridden EMPTY in MC
        // (handleAirSupply {}), which is why Dolphin derives PathfinderMob's
        // BaseTick untouched here: on land the moistness clock is the killer.
        int GetMaxAirSupply() const override { return 4800; }
        int IncreaseAirSupply(int) override { return GetMaxAirSupply(); }

        // MC Dolphin.getMaxHeadXRot/YRot: 1 — the whole body steers.
        int GetMaxHeadXRot() const override { return 1; }
        int GetMaxHeadYRot() const override { return 1; }

        // MC Dolphin.tick — the moistness clock: 2400 ticks out of water
        // (isInWaterOrRain in MC; no rain query here), then 1 damage per
        // tick, and the beached flop (random hop + spin). The client-side
        // DOLPHIN trail particles wait on the particle system.
        void Tick() override;

    protected:
        void RegisterGoals() override;

    public:
        // MC Dolphin.getMoistnessLevel / setMoistnessLevel — the "Moistness"
        // save key. A dolphin left out of water dies on this clock, so a save
        // that resets it to full is a stay of execution vanilla does not give.
        int  GetMoistness() const { return m_moistnessLevel; }
        void SetMoistness(int level) { m_moistnessLevel = level; }

    private:
        int m_moistnessLevel = 2400;   // MC TOTAL_MOISTNESS_LEVEL
    };

    // MC Squid / GlowSquid — no navigation, no move control: the tentacle
    // pump IS the locomotion. Each pump cycle's final quarter sets velocity
    // to the chosen movement vector outright; between pumps the squid coasts
    // at 0.9 drag. Travel is overridden to raw movement — a squid takes no
    // block friction and no gravity while it swims.
    // Like the dolphin an AgeableWaterCreature in MC: 5% of a pack past the
    // first member spawns as a baby (Squid.finalizeSpawn), the baby box is
    // 0.5 x 0.5 (BABY_DIMENSIONS) and the ink burst shrinks with it.
    class Squid : public AgeableMob {
    public:
        Squid(EntityTypeId type, EntityLevel* level);

        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Squid is a WaterAnimal: a beached squid suffocates on the same
        // clock as a beached fish (see HandleWaterAnimalAirSupply).
        void BaseTick() override;

        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1..3.
        int GetXpReward() const override;

        bool HasMovementVector() const {
            return glm::dot(m_movementVector, m_movementVector) > 1.0e-10;
        }
        void SetMovementVector(const glm::dvec3& v) { m_movementVector = v; }

        void AiStep() override;
        void Travel(const glm::dvec3& input) override;

        // MC hurtServer sprays ink and flees; the flee half is the goal's,
        // the ink cloud is a particle system away.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC Squid.handleEntityEvent(19) — the pump-wrap reset. The server
        // broadcasts it once per stroke; the client pins its own clock at 2π
        // until the event arrives, which keeps both sides' strokes in phase.
        void HandleEntityEvent(uint8_t id) override;

        // MC SquidRenderer.extractRenderState: tentacleAngle lerped by the
        // partial tick.
        float GetTentacleAngle(float partialTick) const {
            return m_oldTentacleAngle +
                   partialTick * (m_tentacleAngle - m_oldTentacleAngle);
        }

    private:
        glm::dvec3 m_movementVector{0.0};
        float m_tentacleMovement = 0.0f;
        float m_oldTentacleMovement = 0.0f;
        float m_tentacleAngle = 0.0f;
        float m_oldTentacleAngle = 0.0f;
        float m_tentacleSpeed = 0.0f;
        float m_rotateSpeed = 0.0f;
    };

    // ── Nautilus / ZombieNautilus ──────────────────────────────────────────
    //
    // MC animal/nautilus/AbstractNautilus and its two concretes, on the
    // ported NautilusAi / ZombieNautilusAi brains. MC's AbstractNautilus is a
    // TamableAnimal + PlayerRideableJumping with a shell inventory; the
    // taming (mobInteract feeding per tag), riding (saddle, rider dash, the
    // rider's water-breathing aura) and inventory halves are SKIPPED — item,
    // equipment and riding-input systems. What remains is MC's real shape:
    // the brain, the anger-on-hurt, the nautilus air rule (dries out of
    // water, never drowns), and the def's smooth-swim locomotion.
    class AbstractNautilus : public GenericAnimal {
    public:
        AbstractNautilus(EntityTypeId type, EntityLevel* level);

        // MC AbstractNautilus.getWalkTargetValue — flat 0 (water is home).
        float GetWalkTargetValue(const glm::ivec3&) const override { return 0.0f; }

        // MC Nautilus.getMaxAirSupply — 300, refilled in water, drained on
        // land with the dry-out damage (handleAirSupply in BaseTick).
        int GetMaxAirSupply() const override { return 300; }
        void BaseTick() override;

        // MC AbstractNautilus.hurtServer — anger at whatever hit it.
        bool Hurt(MobDamageSource source, float amount, Entity* attacker) override;

        // MC AbstractNautilus.canBeAffected — immune to poison (it eats
        // pufferfish for lunch).
        bool CanBeAffected(const MobEffectInstance& effect) const override;

        // MC AbstractNautilus.finalizeSpawn → NautilusAi.initMemories.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC AbstractNautilus.removeWhenFarAway — true (unless tame; no
        // taming system).
        bool RemoveWhenFarAway(double) const override { return true; }
    };

    class Nautilus : public AbstractNautilus {
    public:
        explicit Nautilus(EntityLevel* level);
        void UpdateBrainActivity() override;
    };

    // MC ZombieNautilus — never a baby, never breeds, and hostile through the
    // shared target finder. Its biome texture variant (temperate/cold/warm)
    // is SKIPPED: the port ships one zombie_nautilus.png.
    class ZombieNautilus : public AbstractNautilus {
    public:
        explicit ZombieNautilus(EntityLevel* level);
        void UpdateBrainActivity() override;

        bool IsBaby() const override { return false; }

        // MC ZombieNautilus.getBreedOffspring — null: no zombie babies. The
        // brain has no AnimalMakeLove either, so this is belt and braces.
        std::unique_ptr<Animal> CreateBaby() override { return nullptr; }
    };

} // namespace Game
