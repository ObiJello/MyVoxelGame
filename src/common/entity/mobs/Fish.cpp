// File: src/common/entity/mobs/Fish.cpp
#include "common/entity/mobs/Fish.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/entity/ai/goals/FishGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/goals/DolphinGoals.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/brain/Brain.hpp"
#include "common/entity/ai/brain/NautilusAi.hpp"
#include "common/entity/ai/brain/ZombieNautilusAi.hpp"
#include "common/entity/ai/navigation/WaterBoundPathNavigation.hpp"
#include "common/entity/effect/MobEffects.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"

namespace Game {

    // ── Fish (MC AbstractFish) ─────────────────────────────────────────────

    Fish::Fish(EntityTypeId type, EntityLevel* level) : PathfinderMob(type, level) {
        // MC AbstractFish's constructor wiring: MAX_HEALTH 3, the fish move
        // control and the water-bound navigation, WATER path malus 0.
        m_attributes.Register(Attribute::MaxHealth, 3.0);
        m_health = GetMaxHealth();
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<FishMoveControl>(this));
        SetNavigation(std::make_unique<WaterBoundPathNavigation>(this, level));
    }

    void Fish::RegisterGoals() {
        // MC AbstractFish.registerGoals: panic 1.25 at 0, flee non-spectating
        // players (8 blocks, 1.6 walking, 1.4 sprinting) at 2, the gated swim
        // wander at 4.
        m_goalSelector.AddGoal(0, std::make_unique<PanicGoal>(this, 1.25));
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(this, 8.0f, 1.6, 1.4));
        m_goalSelector.AddGoal(4, std::make_unique<FishSwimGoal>(this));
    }

    void HandleWaterAnimalAirSupply(Mob& mob, int preTickAirSupply) {
        // MC WaterAnimal.handleAirSupply, verbatim (server-side; the caller
        // gates). Note the reset is to the constant 300, NOT getMaxAirSupply.
        if (mob.Level() && mob.Level()->IsClientSide()) return;
        if (mob.IsAlive() && !mob.IsInWater()) {
            mob.SetAirSupply(preTickAirSupply - 1);
            if (mob.GetAirSupply() <= -20) {
                mob.SetAirSupply(0);
                mob.Hurt(MobDamageSource::Drown, 2.0f, nullptr);
            }
        } else {
            mob.SetAirSupply(300);
        }
    }

    void Fish::BaseTick() {
        // MC WaterAnimal.baseTick: capture the PRE-tick air (super's own air
        // block would refill a beached fish), then invert it.
        const int airSupply = GetAirSupply();
        PathfinderMob::BaseTick();
        HandleWaterAnimalAirSupply(*this, airSupply);
    }

    int Fish::GetXpReward() const {
        // MC WaterAnimal.getBaseExperienceReward (WaterAnimal.java:32-34):
        // 1 + nextInt(3) — the same 1..3 an Animal pays.
        return m_level ? 1 + m_level->Random().NextInt(3) : 1;
    }

    void Fish::AiStep() {
        // MC: the beached flop — grounded out of water, hop 0.4 up with a
        // random sideways jerk each landing.
        if (!IsInWater() && onGround && verticalCollision) {
            JavaRandom& rng = m_level->Random();
            velocity.x += (rng.NextFloat() * 2.0f - 1.0f) * 0.05f;
            velocity.y += 0.4;
            velocity.z += (rng.NextFloat() * 2.0f - 1.0f) * 0.05f;
            onGround = false;
            needsSync = true;
        }
        PathfinderMob::AiStep();
    }

    // ── SchoolingFish (MC AbstractSchoolingFish) ───────────────────────────

    namespace {
        // MC SchoolSpawnGroupData — the pack token carrying the leader.
        struct SchoolSpawnGroupData : SpawnGroupData {
            explicit SchoolSpawnGroupData(SchoolingFish* l) : leader(l) {}
            SchoolingFish* leader;
        };
    }

    SchoolingFish::SchoolingFish(EntityTypeId type, EntityLevel* level)
        : Fish(type, level) {
        RegisterGoals();
    }

    void SchoolingFish::RegisterGoals() {
        Fish::RegisterGoals();
        m_goalSelector.AddGoal(5, std::make_unique<FollowFlockLeaderGoal>(this));
    }

    void SchoolingFish::PathToLeader() {
        if (IsFollower()) {
            GetNavigation().MoveTo(*m_leader, 1.0);
        }
    }

    void SchoolingFish::Tick() {
        Fish::Tick();

        // MC: a leader occasionally re-checks whether its school still exists
        // — alone again means schoolSize resets to 1.
        if (HasFollowers() && m_level && m_level->Random().NextInt(200) == 1) {
            AABB box = GetAABB();
            box.min -= glm::vec3(8.0f);
            box.max += glm::vec3(8.0f);
            std::vector<Entity*> nearby;
            m_level->GetEntitiesInBox(box, this, nearby);
            int sameType = 0;
            for (Entity* e : nearby) {
                if (e->GetType() == GetType()) ++sameType;
            }
            if (sameType < 1) m_schoolSize = 1;
        }
    }

    // ── Pufferfish ─────────────────────────────────────────────────────────

    Pufferfish::Pufferfish(EntityLevel* level)
        : Fish(EntityTypeId::Pufferfish, level) {
        RegisterGoals();
        // MC's constructor also calls refreshDimensions() — the per-state
        // model scale (0.5/0.7/1.0) is entity-dimension work the type table
        // does not carry; the collision box stays the table's.
    }

    void Pufferfish::RegisterGoals() {
        // MC Pufferfish.registerGoals: super's fish set, plus the puff goal
        // at priority 1.
        Fish::RegisterGoals();
        m_goalSelector.AddGoal(1, std::make_unique<PufferfishPuffGoal>(this));
    }

    bool Pufferfish::IsScaryTarget(const LivingEntity& target) {
        // MC SCARY_MOB: creative players are not scary, and neither is
        // anything in EntityTypeTags.NOT_SCARY_FOR_PUFFERFISH. The tag's
        // vanilla members (transcribed from the 1.21 data pack — the JSONs
        // are not in the decompiled tree) are the aquatic neighbours below.
        // Spectators are additionally excluded — MC's TargetingConditions
        // never test a spectator.
        if (target.IsCreative() || target.IsSpectator()) return false;
        switch (target.GetType()) {
            case EntityTypeId::Axolotl:
            case EntityTypeId::Cod:
            case EntityTypeId::Dolphin:
            case EntityTypeId::GlowSquid:
            case EntityTypeId::Pufferfish:
            case EntityTypeId::Salmon:
            case EntityTypeId::Squid:
            case EntityTypeId::Tadpole:
            case EntityTypeId::TropicalFish:
            case EntityTypeId::Turtle:
                return false;
            default:
                return true;
        }
    }

    void Pufferfish::Tick() {
        // MC Pufferfish.tick: the state machine runs BEFORE super, server
        // side, alive, effective AI. (The blow-up/blow-out sounds wait on the
        // sound system.)
        if (m_level && !m_level->IsClientSide() && IsAlive() && IsEffectiveAi()) {
            if (m_inflateCounter > 0) {
                if (m_puffState == kStateSmall) {
                    m_puffState = kStateMid;
                } else if (m_inflateCounter > 40 && m_puffState == kStateMid) {
                    m_puffState = kStateFull;
                }
                ++m_inflateCounter;
            } else if (m_puffState != kStateSmall) {
                if (m_deflateTimer > 60 && m_puffState == kStateFull) {
                    m_puffState = kStateMid;
                } else if (m_deflateTimer > 100 && m_puffState == kStateMid) {
                    m_puffState = kStateSmall;
                }
                ++m_deflateTimer;
            }
        }
        Fish::Tick();
    }

    void Pufferfish::Touch(LivingEntity& mob) {
        // MC Pufferfish.touch / playerTouch — identical numbers for both:
        // (1 + state) mob-attack damage, then POISON for 60 * state ticks
        // (amplifier 0). playerTouch additionally sends the PUFFER_FISH_STING
        // game event — a client-side sting sound; no such packet exists.
        // (NAUSEA belongs to EATING a pufferfish, not the sting — nothing to
        // skip here.)
        const int state = m_puffState;
        if (mob.Hurt(MobDamageSource::MobAttack,
                     static_cast<float>(1 + state), this)) {
            mob.AddEffect(MobEffectInstance(MobEffectId::Poison, 60 * state, 0),
                          this);
        }
    }

    void Pufferfish::AiStep() {
        // MC Pufferfish.aiStep: super first, then — while puffed — sting
        // everything scary within the box inflated by 0.3. MC splits players
        // out into playerTouch (the collision callback); one sweep over
        // LivingEntities covers both here, since the player views sit in the
        // same entity query — same box, same numbers, same cadence through
        // the victim's i-frames.
        Fish::AiStep();
        if (m_level && !m_level->IsClientSide() && IsAlive() && m_puffState > 0) {
            AABB box = GetAABB();
            box.min -= glm::vec3(0.3f);
            box.max += glm::vec3(0.3f);

            std::vector<Entity*> nearby;
            m_level->GetEntitiesInBox(box, this, nearby);
            for (Entity* e : nearby) {
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living || !living->IsAlive()) continue;
                if (!IsScaryTarget(*living)) continue;
                Touch(*living);
            }
        }
    }

    // ── Dolphin ────────────────────────────────────────────────────────────

    namespace {

        constexpr EntityTypeId kDolphinGuardianAvoid[] = {
            EntityTypeId::Guardian, EntityTypeId::ElderGuardian,
        };

        // MC registers `new HurtByTargetGoal(this, Guardian.class)` — the
        // ignored-class form: a dolphin hurt BY a guardian does not
        // retaliate (it flees via the avoid goal); anything else gets the
        // pod on it (setAlertOthers). The shared goal has no ignore list, so
        // the filter lives in this file-local subclass.
        class DolphinHurtByTargetGoal : public HurtByTargetGoal {
        public:
            explicit DolphinHurtByTargetGoal(Dolphin* dolphin)
                : HurtByTargetGoal(dolphin), m_dolphin(dolphin) {
                SetAlertOthers();
            }

            bool CanUse() override {
                Entity* attacker = m_dolphin->GetLastHurtByMob();
                if (attacker
                    && (attacker->GetType() == EntityTypeId::Guardian
                        || attacker->GetType() == EntityTypeId::ElderGuardian)) {
                    return false;
                }
                return HurtByTargetGoal::CanUse();
            }

            const char* Name() const override { return "DolphinHurtByTargetGoal"; }

        private:
            Dolphin* m_dolphin;
        };

    } // namespace

    void Dolphin::CreateAttributes(AttributeMap& out) {
        // MC Dolphin.createAttributes on the MOB base (not animal):
        // MAX_HEALTH 10, MOVEMENT_SPEED 1.2, ATTACK_DAMAGE 3.
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::MovementSpeed, 1.2);
        out.Register(Attribute::AttackDamage,  3.0);
    }

    namespace {
        // MC AgeableMob.AgeableMobGroupData for the water creatures — the
        // first pack member spawns adult, later members roll babyChance
        // (the animals keep their own copy of this token in Animals.cpp).
        struct WaterAgeableGroupData : SpawnGroupData {
            explicit WaterAgeableGroupData(float chance) : babyChance(chance) {}
            float babyChance;
            int   size = 0;
        };

        // MC AgeableMob.finalizeSpawn's roll, with the chance the concrete
        // class seeds when no pack token exists yet.
        void RollPackBaby(AgeableMob& mob, EntityLevel* level, float chance,
                          std::shared_ptr<SpawnGroupData>& groupData) {
            if (!level) return;
            auto* data = dynamic_cast<WaterAgeableGroupData*>(groupData.get());
            if (!data) {
                groupData = std::make_shared<WaterAgeableGroupData>(chance);
                data = static_cast<WaterAgeableGroupData*>(groupData.get());
            }
            if (data->size > 0 && level->Random().NextFloat() <= data->babyChance) {
                mob.SetAge(AgeableMob::kBabyStartAge);
            }
            ++data->size;
        }
    } // namespace

    std::shared_ptr<SpawnGroupData>
    Dolphin::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Dolphin.finalizeSpawn: setAirSupply(max), xRot 0, then the
        // AgeableMobGroupData(0.1F) roll.
        SetAirSupply(GetMaxAirSupply());
        xRot = 0.0f;
        RollPackBaby(*this, m_level, 0.1f, groupData);
        return AgeableMob::FinalizeSpawn(reason, std::move(groupData));
    }

    std::shared_ptr<SpawnGroupData>
    Squid::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Squid.finalizeSpawn: AgeableMobGroupData(0.05F).
        RollPackBaby(*this, m_level, 0.05f, groupData);
        return AgeableMob::FinalizeSpawn(reason, std::move(groupData));
    }

    Dolphin::Dolphin(EntityLevel* level)
        : AgeableMob(EntityTypeId::Dolphin, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: setAirSupply(getMaxAirSupply()) — the Entity
        // default is 300, a dolphin carries 4800.
        SetAirSupply(GetMaxAirSupply());

        // MC's constructor wiring: the smooth-swim controls and the
        // water-bound navigation; water paths are free (AgeableWaterCreature
        // sets the malus). setCanPickUpLoot rides the mob item system.
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<SmoothSwimmingMoveControl>(
            this, 85, 10, 0.02f, 0.1f, true));
        SetLookControl(std::make_unique<SmoothSwimmingLookControl>(this, 10));
        SetNavigation(std::make_unique<WaterBoundPathNavigation>(this, level));

        RegisterGoals();
    }

    void Dolphin::RegisterGoals() {
        // MC Dolphin.registerGoals, priority for priority. BreathAirGoal is
        // live now that the air supply exists; the still-inert entries
        // (treasure, swim-with-player, item play, boat following) say why at
        // their declarations in DolphinGoals.hpp. MC registers
        // FollowPlayerRiddenEntityGoal twice (boats, nautiluses); one inert
        // registration stands for both.
        m_goalSelector.AddGoal(0, std::make_unique<BreathAirGoal>(this));
        m_goalSelector.AddGoal(0, std::make_unique<TryFindWaterGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<DolphinSwimToTreasureGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<DolphinSwimWithPlayerGoal>(this, 4.0));
        m_goalSelector.AddGoal(4, std::make_unique<RandomSwimmingGoal>(this, 1.0, 10));
        m_goalSelector.AddGoal(4, std::make_unique<RandomLookAroundGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(5, std::make_unique<DolphinJumpGoal>(this, 10));
        m_goalSelector.AddGoal(6, std::make_unique<MeleeAttackGoal>(this, 1.2, true));
        m_goalSelector.AddGoal(8, std::make_unique<PlayWithItemsGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<FollowPlayerRiddenEntityGoal>(this));
        m_goalSelector.AddGoal(9, std::make_unique<AvoidEntityGoal>(
                                      this, kDolphinGuardianAvoid, 2, 8.0f, 1.0, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<DolphinHurtByTargetGoal>(this));
    }

    int Dolphin::GetXpReward() const {
        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1 + nextInt(3).
        return m_level ? 1 + m_level->Random().NextInt(3) : 1;
    }

    void Dolphin::Tick() {
        AgeableMob::Tick();

        if (IsNoAi()) {
            // MC Dolphin.tick: a no-AI dolphin neither drowns nor dries.
            SetAirSupply(GetMaxAirSupply());
            return;
        }
        if (!m_level || m_level->IsClientSide()) {
            // MC's client half is the DOLPHIN trail particles — none yet.
            return;
        }

        // MC's moistness clock (isInWaterOrRain — no rain query here).
        if (IsInWater()) {
            m_moistnessLevel = 2400;
        } else {
            --m_moistnessLevel;
            if (m_moistnessLevel <= 0) {
                // MC damageSources().dryOut() — 1 per tick until it dies.
                Hurt(MobDamageSource::Generic, 1.0f, nullptr);
            }

            if (onGround) {
                // The beached flop: a random hop with a random new heading.
                JavaRandom& rng = m_level->Random();
                velocity += glm::dvec3(
                    static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.2f),
                    0.5,
                    static_cast<double>((rng.NextFloat() * 2.0f - 1.0f) * 0.2f));
                yRot = rng.NextFloat() * 360.0f;
                onGround = false;
                needsSync = true;
            }
        }
    }

    // ── Squid ──────────────────────────────────────────────────────────────

    int Squid::GetXpReward() const {
        // MC AgeableWaterCreature.getBaseExperienceReward
        // (AgeableWaterCreature.java:30-32): 1 + nextInt(3).
        return m_level ? 1 + m_level->Random().NextInt(3) : 1;
    }

    void Squid::BaseTick() {
        // MC WaterAnimal.baseTick (Squid extends AgeableWaterCreature): a
        // beached squid suffocates exactly like a beached fish.
        const int airSupply = GetAirSupply();
        Mob::BaseTick();
        HandleWaterAnimalAirSupply(*this, airSupply);
    }

    Squid::Squid(EntityTypeId type, EntityLevel* level) : AgeableMob(type, level) {
        m_attributes.Register(Attribute::MaxHealth, 10.0);
        m_health = GetMaxHealth();
        if (level) {
            m_tentacleSpeed = 1.0f / (level->Random().NextFloat() + 1.0f) * 0.2f;
        }
        m_goalSelector.AddGoal(0, std::make_unique<SquidRandomMovementGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<SquidFleeGoal>(this));
    }

    void Squid::Travel(const glm::dvec3&) {
        // MC Squid.travel: raw movement, no friction, no gravity — the pump
        // cycle below is the only thing that writes velocity in water.
        Move(velocity);
    }

    void Squid::AiStep() {
        AgeableMob::AiStep();

        const bool serverSide = m_level && !m_level->IsClientSide();

        // MC saves last tick's values right after super.aiStep() — the
        // renderer lerps the tentacle angle between them.
        m_oldTentacleMovement = m_tentacleMovement;
        m_oldTentacleAngle = m_tentacleAngle;

        // The tentacle pump phase (also the client's animation clock).
        m_tentacleMovement += m_tentacleSpeed;
        constexpr float kTwoPi = 2.0f * 3.14159265358979323846f;
        constexpr float kPi = 3.14159265358979323846f;
        if (m_tentacleMovement > kTwoPi) {
            if (!serverSide) {
                m_tentacleMovement = kTwoPi;
            } else {
                m_tentacleMovement -= kTwoPi;
                if (m_level->Random().NextInt(10) == 0) {
                    m_tentacleSpeed = 1.0f / (m_level->Random().NextFloat() + 1.0f) * 0.2f;
                }
                // MC: event 19 restarts the client's pump clock each wrap.
                m_level->BroadcastEntityEvent(*this, 19);
            }
        }

        if (IsInWater()) {
            if (m_tentacleMovement < kPi) {
                const float phase = m_tentacleMovement / kPi;
                // MC: tentacleAngle = sin(scale² · π) · π · 0.25 — the curl
                // peaks mid-contraction and relaxes through the stroke.
                m_tentacleAngle = std::sin(phase * phase * kPi) * kPi * 0.25f;
                if (phase > 0.75f) {
                    // The thrust quarter of the stroke: velocity snaps to the
                    // jet vector outright.
                    if (serverSide) velocity = m_movementVector;
                    m_rotateSpeed = 1.0f;
                } else {
                    m_rotateSpeed *= 0.8f;
                }
            } else {
                // Coasting between pumps.
                m_tentacleAngle = 0.0f;
                if (serverSide) velocity *= 0.9;
                m_rotateSpeed *= 0.99f;
            }

            // Face the way it moves.
            const double horiz = std::sqrt(velocity.x * velocity.x +
                                           velocity.z * velocity.z);
            if (horiz > 1.0e-8 || std::abs(velocity.y) > 1.0e-8) {
                yBodyRot += (-static_cast<float>(std::atan2(velocity.x, velocity.z)) *
                                 57.29577951308232f - yBodyRot) * 0.1f;
                yRot = yBodyRot;
            }
        } else {
            // MC: out of water the tentacles thrash with the raw pump clock —
            // on both sides, it is the beached-squid flail.
            m_tentacleAngle = std::abs(std::sin(m_tentacleMovement)) * kPi * 0.25f;
            if (serverSide) {
                // Beached: fall straight down, no drifting — unless levitating
                // (MC Squid.aiStep's LEVITATION branch replaces gravity with
                // the fixed 0.05 * (amp + 1) rise).
                double yd;
                if (const MobEffectInstance* lev = GetEffect(MobEffectId::Levitation)) {
                    yd = 0.05 * static_cast<double>(lev->amplifier + 1);
                } else {
                    yd = velocity.y - GetGravity();
                }
                velocity = glm::dvec3(0.0, yd * 0.98, 0.0);
            }
        }
    }

    void Squid::HandleEntityEvent(uint8_t id) {
        // MC Squid.handleEntityEvent: `if (id == 19) this.tentacleMovement = 0.0F;`
        if (id == 19) {
            m_tentacleMovement = 0.0f;
            return;
        }
        AgeableMob::HandleEntityEvent(id);
    }

    bool Squid::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC sprays an ink cloud on a successful hit; the flee goal reads the
        // lastHurtByMob this sets. STILL SKIPPED with the particle system
        // landed: SQUID_INK is its own particle class (SquidInkParticle —
        // animated squid_ink sheet, water-drag tick) and the sprite is not
        // in assets/textures/particle/; add both to MobParticleSystem when
        // the ink visual is wanted.
        return AgeableMob::Hurt(source, amount, attacker);
    }

    std::shared_ptr<SpawnGroupData>
    SchoolingFish::FinalizeSpawn(SpawnReason reason,
                                 std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Fish::FinalizeSpawn(reason, std::move(groupData));
        if (!groupData) {
            groupData = std::make_shared<SchoolSpawnGroupData>(this);
        } else if (auto* school = dynamic_cast<SchoolSpawnGroupData*>(groupData.get())) {
            // NOTE the dangling-leader hazard the raw pointer carries: the
            // token only lives for one spawn pack, inside one tick, and the
            // leader was added to the level before any follower finalizes —
            // the same lifetime MC relies on.
            if (school->leader && school->leader != this) {
                StartFollowing(school->leader);
            }
        }
        return groupData;
    }

    std::shared_ptr<SpawnGroupData>
    TropicalFish::FinalizeSpawn(SpawnReason reason,
                                std::shared_ptr<SpawnGroupData> groupData) {
        // MC TropicalFish.finalizeSpawn (TropicalFish.java:197-218): a fish
        // continuing an existing pack inherits the pack's variant and stays a
        // school; a pack INITIATOR rolls nextFloat() — under 0.9 it starts a
        // common-variant school, otherwise it takes a rare variant and
        // `isSchool = false` (the loner). Variant draws are not modelled (one
        // tropical fish texture), so only the school/loner consequence of the
        // roll survives. MC checks instanceof BEFORE its variant logic; the
        // pack test happens before super here for the same reason — super
        // creates the group token that would otherwise mask "initiator".
        const bool packContinuation =
            dynamic_cast<SchoolSpawnGroupData*>(groupData.get()) != nullptr;

        groupData = SchoolingFish::FinalizeSpawn(reason, std::move(groupData));

        if (!packContinuation && m_level &&
            m_level->Random().NextFloat() >= 0.9f) {
            m_isSchool = false;
        }
        return groupData;
    }

    // ── Nautilus / ZombieNautilus ──────────────────────────────────────────

    AbstractNautilus::AbstractNautilus(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level) {
        // NO GOALS — MC's nautili never register any; the brain is the whole
        // behaviour. The def already applied MC's locomotion: water-bound
        // navigation + SmoothSwimmingMoveControl(85, 10, 0.011, 0.0, true)
        // + SmoothSwimmingLookControl(10).
        m_goalSelector.Clear();
        m_targetSelector.Clear();
        SetPathfindingMalus(PathType::Water, 0.0f);
    }

    void AbstractNautilus::BaseTick() {
        // MC Nautilus.baseTick/handleAirSupply — the axolotl's inverted air
        // rule at 300 ticks: dries out on land (2.0/tick once dry), refills
        // in water, never drowns. MC keys on isInWater here, not
        // isInWaterOrRain.
        const int airSupply = GetAirSupply();
        GenericAnimal::BaseTick();
        if (m_level && !m_level->IsClientSide() && !IsNoAi()) {
            if (IsAlive() && !IsInWater()) {
                SetAirSupply(airSupply - 1);
                if (GetAirSupply() <= -20) {
                    SetAirSupply(0);
                    Hurt(MobDamageSource::Drown, 2.0f, nullptr);
                }
            } else {
                SetAirSupply(GetMaxAirSupply());
            }
        }
    }

    bool AbstractNautilus::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        const bool hurt = GenericAnimal::Hurt(source, amount, attacker);
        // MC AbstractNautilus.hurtServer → NautilusAi.setAngerTarget.
        if (hurt && m_level && !m_level->IsClientSide()) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                NautilusAi::SetAngerTarget(*this, *living);
            }
        }
        return hurt;
    }

    bool AbstractNautilus::CanBeAffected(const MobEffectInstance& effect) const {
        // MC AbstractNautilus.canBeAffected — poison immune.
        if (effect.effect == MobEffectId::Poison) return false;
        return GenericAnimal::CanBeAffected(effect);
    }

    std::shared_ptr<SpawnGroupData>
    AbstractNautilus::FinalizeSpawn(SpawnReason reason,
                                    std::shared_ptr<SpawnGroupData> groupData) {
        // MC AbstractNautilus.finalizeSpawn → NautilusAi.initMemories (the
        // rolled unprovoked-attack cooldown). The baby-odds half rides
        // AgeableMob's shared group data, which this port's animals do not
        // thread; the zombie variant's biome-texture pick is skipped with the
        // variant itself.
        NautilusAi::InitMemories(*this);
        return GenericAnimal::FinalizeSpawn(reason, std::move(groupData));
    }

    Nautilus::Nautilus(EntityLevel* level)
        : AbstractNautilus(EntityTypeId::Nautilus, level) {
        m_brain = std::make_unique<Brain>();
        NautilusAi::InitBrain(*this, *m_brain);
    }

    void Nautilus::UpdateBrainActivity() { NautilusAi::UpdateActivity(*this); }

    ZombieNautilus::ZombieNautilus(EntityLevel* level)
        : AbstractNautilus(EntityTypeId::ZombieNautilus, level) {
        m_brain = std::make_unique<Brain>();
        ZombieNautilusAi::InitBrain(*this, *m_brain);
    }

    void ZombieNautilus::UpdateBrainActivity() {
        ZombieNautilusAi::UpdateActivity(*this);
    }

} // namespace Game
