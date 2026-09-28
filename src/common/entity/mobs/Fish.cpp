// File: src/common/entity/mobs/Fish.cpp
#include "common/entity/mobs/Fish.hpp"
#include "common/particle/ParticleOptions.hpp"
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
#include "common/sound/SoundEvents.hpp"
#include "common/entity/Bucketable.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/data/DataComponents.hpp"
#include "common/world/tags/DataTags.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/PlayerRideable.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"
#include "common/network/packets/game/GameEventS2CPacket.hpp"

#include <algorithm>
#include <cmath>

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

    // ── Bucketable (MC AbstractFish) ───────────────────────────────────────

    ItemID Fish::GetBucketItem() const {
        switch (GetType()) {
            case EntityTypeId::Salmon:       return Items::SalmonBucket;
            case EntityTypeId::Pufferfish:   return Items::PufferfishBucket;
            case EntityTypeId::TropicalFish: return Items::TropicalFishBucket;
            case EntityTypeId::Cod:
            default:                         return Items::CodBucket;
        }
    }

    const char* Fish::GetPickupSound() const { return SoundEvents::BUCKET_FILL_FISH; }

    void Fish::SaveToBucket(ItemStack& bucket) const {
        Bucketable::SaveDefaultDataToBucketTag(*this, bucket);
    }

    void Fish::LoadFromBucket(const BucketEntityData& data) {
        Bucketable::LoadDefaultDataFromBucketTag(*this, data);
    }

    UseResult Fish::MobInteract(LivingEntity& player, ItemStack& held) {
        if (auto r = Bucketable::BucketMobPickup(*this, player, held, GetBucketItem(), GetPickupSound(),
                                                 [this](ItemStack& bucket) { SaveToBucket(bucket); })) {
            return *r;
        }
        return PathfinderMob::MobInteract(player, held);
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
            // MC AbstractFish.aiStep: playSound(getFlopSound(), volume, pitch).
            const char* flop = "";
            switch (GetType()) {
                case EntityTypeId::Cod:          flop = SoundEvents::COD_FLOP; break;
                case EntityTypeId::Salmon:       flop = SoundEvents::SALMON_FLOP; break;
                case EntityTypeId::Pufferfish:   flop = SoundEvents::PUFFER_FISH_FLOP; break;
                case EntityTypeId::TropicalFish: flop = SoundEvents::TROPICAL_FISH_FLOP; break;
                default: break;
            }
            PlaySound(flop, GetSoundVolume(), GetVoicePitch());
        }
        PathfinderMob::AiStep();
    }

    bool Fish::TravelInWaterOverride(const glm::dvec3& input, double baseGravity,
                                     bool isFalling, double oldY) {
        // MC AbstractFish.travelInWater, verbatim: none of travelInFluid's
        // captured values are read — no gravity sixteenth, no drag split,
        // no jumpOutOfFluid.
        (void)baseGravity; (void)isFalling; (void)oldY;
        MoveRelative(0.01f, input);
        Move(velocity);
        velocity *= 0.9;
        if (!GetTarget()) velocity.y += -0.005;
        return true;
    }

    // ── SchoolingFish (MC AbstractSchoolingFish) ───────────────────────────

    namespace {
        // MC AbstractSchoolingFish.SchoolSpawnGroupData — the pack token
        // carrying the leader. Held as an EntityRef, not a raw pointer: the
        // leader is normally added to the level before any follower
        // finalizes, but a spawn that fails to add it destroys it, and the
        // ref's liveness token turns that into "no leader" instead of a
        // dangling read.
        struct SchoolSpawnGroupData : SpawnGroupData {
            explicit SchoolSpawnGroupData(SchoolingFish* l) { leader.Set(l); }
            EntityRef leader;
        };

        // MC TropicalFish.TropicalFishGroupData — the school token that also
        // carries the variant every member of the school takes.
        struct TropicalFishGroupData : SchoolSpawnGroupData {
            TropicalFishGroupData(SchoolingFish* l, const TropicalFishVariants::Variant& v)
                : SchoolSpawnGroupData(l), variant(v) {}
            TropicalFishVariants::Variant variant;
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

    SchoolingFish* SchoolingFish::Leader() const {
        if (m_leader.Empty() || !m_level) return nullptr;
        Entity* e = m_leader.Get(*m_level);
        // Only a same-type schooling fish is ever followed (StartFollowing's
        // callers), so the type test stands in for the class test.
        if (!e || e->GetType() != GetType()) return nullptr;
        return static_cast<SchoolingFish*>(e);
    }

    void SchoolingFish::StartFollowing(SchoolingFish& leader) {
        m_leader.Set(&leader);
        MarkHoldsEntityRefs();   // see Entity::HoldsEntityRefs
        ++leader.m_schoolSize;
    }

    void SchoolingFish::StopFollowing() {
        if (SchoolingFish* leader = Leader()) --leader->m_schoolSize;
        m_leader.Clear();
    }

    void SchoolingFish::AddFollowers(const std::vector<SchoolingFish*>& candidates) {
        // MC: stream.limit(getMaxSchoolSize() - schoolSize).filter(f != this)
        // .forEach(f -> f.startFollowing(this)).
        const int limit = GetMaxSchoolSize() - m_schoolSize;
        int taken = 0;
        for (SchoolingFish* fish : candidates) {
            if (taken >= limit) break;
            ++taken;
            if (fish == this) continue;
            fish->StartFollowing(*this);
        }
    }

    void SchoolingFish::PathToLeader() {
        if (!IsFollower()) return;
        if (SchoolingFish* leader = Leader()) GetNavigation().MoveTo(*leader, 1.0);
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
        // side, alive, effective AI; every puff edge voices blow-up/blow-out.
        if (m_level && !m_level->IsClientSide() && IsAlive() && IsEffectiveAi()) {
            if (m_inflateCounter > 0) {
                if (m_puffState == kStateSmall) {
                    MakeSound(SoundEvents::PUFFER_FISH_BLOW_UP);
                    m_puffState = kStateMid;
                } else if (m_inflateCounter > 40 && m_puffState == kStateMid) {
                    MakeSound(SoundEvents::PUFFER_FISH_BLOW_UP);
                    m_puffState = kStateFull;
                }
                ++m_inflateCounter;
            } else if (m_puffState != kStateSmall) {
                if (m_deflateTimer > 60 && m_puffState == kStateFull) {
                    MakeSound(SoundEvents::PUFFER_FISH_BLOW_OUT);
                    m_puffState = kStateMid;
                } else if (m_deflateTimer > 100 && m_puffState == kStateMid) {
                    MakeSound(SoundEvents::PUFFER_FISH_BLOW_OUT);
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
        // (amplifier 0). A mob's sting is a broadcast playSound; a player's
        // is the PUFFER_FISH_STING game event to that player alone (unless
        // this fish is silent), which its client plays at itself — nobody
        // else hears it. playerTouch sends it BEFORE the poison.
        // (NAUSEA belongs to EATING a pufferfish, not the sting — nothing to
        // skip here.)
        const int state = m_puffState;
        if (mob.Hurt(MobDamageSource::MobAttack,
                     static_cast<float>(1 + state), this)) {
            if (mob.IsPlayer()) {
                if (!IsSilent() && m_level) {
                    m_level->SendGameEvent(mob, Network::GameEventS2CPacket::kPufferFishSting, 0.0f);
                }
                mob.AddEffect(MobEffectInstance(MobEffectId::Poison, 60 * state, 0), this);
            } else {
                mob.AddEffect(MobEffectInstance(MobEffectId::Poison, 60 * state, 0), this);
                PlaySound(SoundEvents::PUFFER_FISH_STING, 1.0f, 1.0f);
            }
        }
    }

    void Pufferfish::AiStep() {
        // MC Pufferfish.aiStep: super first, then — while puffed — sting
        // every scary MOB (Mob.class: not a player, not an armor stand)
        // within the fish's box inflated by 0.3. Players are MC's
        // playerTouch, reached from Player.aiStep's touch sweep: the
        // PLAYER's box inflated (1.0, 0.5, 1.0) against this fish's box —
        // a wider reach than a mob's. One sweep here covers both halves (the
        // player views sit in the same entity query), each tested against
        // its own box; same numbers, same cadence through the victim's
        // i-frames.
        Fish::AiStep();
        if (m_level && !m_level->IsClientSide() && IsAlive() && m_puffState > 0) {
            const AABB self = GetAABB();
            AABB mobReach = self;
            mobReach.min -= glm::vec3(0.3f);
            mobReach.max += glm::vec3(0.3f);
            AABB playerReach = self;
            playerReach.min -= glm::vec3(1.0f, 0.5f, 1.0f);
            playerReach.max += glm::vec3(1.0f, 0.5f, 1.0f);

            std::vector<Entity*> nearby;
            m_level->GetEntitiesInBox(playerReach, this, nearby);
            for (Entity* e : nearby) {
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living || !living->IsAlive()) continue;
                if (living->GetType() == EntityTypeId::ArmorStand) continue;   // not a Mob
                if (!IsScaryTarget(*living)) continue;
                const AABB& reach = living->IsPlayer() ? playerReach : mobReach;
                if (!reach.Intersects(living->GetAABB())) continue;
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
        // sets the malus); setCanPickUpLoot(true).
        SetCanPickUpLoot(true);
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<SmoothSwimmingMoveControl>(
            this, 85, 10, 0.02f, 0.1f, true));
        SetLookControl(std::make_unique<SmoothSwimmingLookControl>(this, 10));
        SetNavigation(std::make_unique<WaterBoundPathNavigation>(this, level));

        RegisterGoals();
    }

    void Dolphin::RegisterGoals() {
        // MC Dolphin.registerGoals, priority for priority. The still-inert
        // entries (the treasure hunt's structure search, boat following)
        // say why at their declarations in DolphinGoals.hpp. MC registers
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
        m_goalSelector.AddGoal(7, std::make_unique<DolphinMoveToItemGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<PlayWithItemsGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<FollowPlayerRiddenEntityGoal>(this));
        m_goalSelector.AddGoal(9, std::make_unique<AvoidEntityGoal>(
                                      this, kDolphinGuardianAvoid, 2, 8.0f, 1.0, 1.0));
        m_targetSelector.AddGoal(1, std::make_unique<DolphinHurtByTargetGoal>(this));
    }

    void Dolphin::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        if (!GetEquipment(EquipmentSlot::MAINHAND).IsEmpty() || !CanHoldItem(stack)) return;
        OnItemPickup(itemEntityId, stack);
        SetEquipment(EquipmentSlot::MAINHAND, stack);
        SetGuaranteedDrop(EquipmentSlot::MAINHAND);
        TakeItemEntity(itemEntityId, stack.count);   // take + discard
    }

    UseResult Dolphin::MobInteract(LivingEntity& player, ItemStack& held) {
        if (!held.IsEmpty() &&
            DataTags::HasTag(DataTags::Registry::Item, ItemRegistry::Slug(held.itemId), "minecraft:fishes")) {
            if (m_level && !m_level->IsClientSide()) PlaySound(SoundEvents::DOLPHIN_EAT, 1.0f, 1.0f);
            if (CanAgeUp()) {
                Animal::UsePlayerItem(held);
                AgeUp(GetSpeedUpSecondsWhenFeeding(-GetAge()), /*forced=*/true);
            } else {
                SetGotFish(true);
                Animal::UsePlayerItem(held);
            }
            return UseResult::Success;
        }
        return AgeableMob::MobInteract(player, held);
    }

    bool Dolphin::DropHeldItem() {
        const ItemStack held = GetEquipment(EquipmentSlot::MAINHAND);
        if (held.IsEmpty()) return false;
        if (m_level && !m_level->IsClientSide()) {
            // ItemGoal.drop: from (x, eyeY - 0.3, z), 0.3 along the facing
            // (pitch-scaled; up by sin(xRot) * 1.5) plus a small random
            // sideways kick, pickup delay 40.
            JavaRandom& r = m_level->Random();
            const float dir = r.NextFloat() * 6.2831855f;
            const float pow2 = 0.02f * r.NextFloat();
            const float yr = yRot * 0.017453292f, xr = xRot * 0.017453292f;
            const glm::dvec3 velocity(
                0.3f * -std::sin(yr) * std::cos(xr) + std::cos(dir) * pow2,
                0.3f * std::sin(xr) * 1.5f,
                0.3f * std::cos(yr) * std::cos(xr) + std::sin(dir) * pow2);
            m_level->SpawnThrownItem(glm::dvec3(position.x, GetEyeY() - 0.30000001192092896, position.z),
                                     velocity, held, 40);
        }
        SetEquipment(EquipmentSlot::MAINHAND, ItemStack{});
        return true;
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
        if (!m_level) return;
        if (m_level->IsClientSide()) {
            // MC's client half: while it swims fast, two pairs of DOLPHIN
            // trail particles behind the view, 0.3 either side.
            glm::dvec3 motion = velocity;
            if (glm::dot(motion, motion) < 1.0e-8) motion = position - oldPosition;
            if (IsInWater() && glm::dot(motion, motion) > 0.03) {
                const glm::vec3 view = Mth::ViewVector(xRot, yRot);
                const float c = std::cos(yRot * 0.017453292f) * 0.3f;
                const float sn = std::sin(yRot * 0.017453292f) * 0.3f;
                const float multiplier = 1.2f - m_level->Random().NextFloat() * 0.7f;
                for (int i = 0; i < 2; ++i) {
                    m_level->AddParticle(ParticleKind::Dolphin, position.x - view.x * multiplier + c,
                                         position.y - view.y, position.z - view.z * multiplier + sn, 0.0, 0.0, 0.0);
                    m_level->AddParticle(ParticleKind::Dolphin, position.x - view.x * multiplier - c,
                                         position.y - view.y, position.z - view.z * multiplier - sn, 0.0, 0.0, 0.0);
                }
            }
            return;
        }

        // MC's moistness clock: water or rain keeps a dolphin moist.
        if (IsInWaterOrRain()) {
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

        // MC GlowSquid.aiStep: a GLOW mote over the body every tick (the
        // client copy draws it; the dark-ticks dimming is not modelled).
        if (m_level && !serverSide && GetType() == EntityTypeId::GlowSquid) {
            JavaRandom& r = m_level->Random();
            const double w = static_cast<double>(GetBbWidth()), h = static_cast<double>(GetBbHeight());
            m_level->AddParticle(ParticleKind::Glow, position.x + w * (2.0 * r.NextDouble() - 1.0) * 0.6,
                                 position.y + h * r.NextDouble(),
                                 position.z + w * (2.0 * r.NextDouble() - 1.0) * 0.6, 0.0, 0.0, 0.0);
        }

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
        // MC hurtServer: a landed hit with an attacker on record sprays the
        // ink cloud (spawnInk); the flee goal reads the lastHurtByMob this
        // sets.
        if (!AgeableMob::Hurt(source, amount, attacker)) return false;
        if (m_level && !m_level->IsClientSide() && GetLastHurtByMob() != nullptr) SpawnInk();
        return true;
    }

    void Squid::SpawnInk() {
        // MC spawnInk: the squirt, then 30 ink puffs sprayed out of the
        // mantle's underside (rotateVector: xRot by xBodyRot — the engine's
        // squid does not pitch, so 0 — then yRot by -yBodyRot), each sent
        // as a direct particle (count 0) at speed 0.1.
        const bool glow = GetType() == EntityTypeId::GlowSquid;
        MakeSound(glow ? SoundEvents::GLOW_SQUID_SQUIRT : SoundEvents::SQUID_SQUIRT);
        const double ya = -static_cast<double>(yBodyRot) * 0.017453292;
        const double yc = std::cos(ya), ys = std::sin(ya);
        const auto rotate = [&](const glm::dvec3& v) {
            return glm::dvec3(v.x * yc + v.z * ys, v.y, v.z * yc - v.x * ys);
        };
        const glm::dvec3 pos = rotate(glm::dvec3(0.0, -1.0, 0.0)) + position;
        const ParticleOptions ink(glow ? ParticleKind::GlowSquidInk : ParticleKind::SquidInk);
        JavaRandom& r = m_level->Random();
        for (int i = 0; i < 30; ++i) {
            const glm::dvec3 dir = rotate(glm::dvec3(static_cast<double>(r.NextFloat()) * 0.6 - 0.3, -1.0,
                                                     static_cast<double>(r.NextFloat()) * 0.6 - 0.3));
            const float offsetScale = IsBaby() ? 0.1f : 0.3f;
            const glm::dvec3 d = dir * static_cast<double>(offsetScale + r.NextFloat() * 2.0f);
            m_level->SendParticles(ink, false, false, pos.x, pos.y + 0.5, pos.z, 0, d.x, d.y, d.z,
                                   0.10000000149011612);
        }
    }

    std::shared_ptr<SpawnGroupData>
    SchoolingFish::FinalizeSpawn(SpawnReason reason,
                                 std::shared_ptr<SpawnGroupData> groupData) {
        // MC AbstractSchoolingFish.finalizeSpawn: super, then lead a new
        // pack or follow the pack's leader.
        groupData = Fish::FinalizeSpawn(reason, std::move(groupData));
        if (!groupData) {
            groupData = std::make_shared<SchoolSpawnGroupData>(this);
        } else if (auto* school = dynamic_cast<SchoolSpawnGroupData*>(groupData.get())) {
            Entity* leader = m_level ? school->leader.Get(*m_level) : nullptr;
            // Same type as the pack — a pack is one spawn entry's type.
            if (leader && leader != this && leader->GetType() == GetType()) {
                StartFollowing(*static_cast<SchoolingFish*>(leader));
            }
        }
        return groupData;
    }

    // ── TropicalFish (MC TropicalFish) ─────────────────────────────────────

    std::shared_ptr<SpawnGroupData>
    TropicalFish::FinalizeSpawn(SpawnReason reason,
                                std::shared_ptr<SpawnGroupData> groupData) {
        // MC TropicalFish.finalizeSpawn: super first (which makes the pack
        // token for a pack's first fish, or follows the token's leader),
        // then the variant: a TropicalFishGroupData hands down its school's
        // variant; otherwise nextFloat() < 0.9 picks a named common variant
        // and REPLACES the token so the rest of the pack takes it, and the
        // remaining tenth is a fully random loner (isSchool = false, which
        // also ends its spawn pack — isMaxGroupSizeReached).
        groupData = SchoolingFish::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level) return groupData;
        JavaRandom& rng = m_level->Random();

        TropicalFishVariants::Variant variant;
        if (auto* school = dynamic_cast<TropicalFishGroupData*>(groupData.get())) {
            variant = school->variant;
        } else if (static_cast<double>(rng.NextFloat()) < 0.9) {
            const auto& common = TropicalFishVariants::CommonVariants();
            variant = common[static_cast<size_t>(rng.NextInt(TropicalFishVariants::kCommonVariantCount))];
            groupData = std::make_shared<TropicalFishGroupData>(this, variant);
        } else {
            m_isSchool = false;
            // Util.getRandom over Pattern.values(), then DyeColor.values()
            // twice — base colour first.
            variant.pattern = static_cast<TropicalFishVariants::Pattern>(
                rng.NextInt(TropicalFishVariants::kPatternCount));
            variant.baseColor = static_cast<uint8_t>(rng.NextInt(TropicalFishVariants::kDyeCount));
            variant.patternColor = static_cast<uint8_t>(rng.NextInt(TropicalFishVariants::kDyeCount));
        }
        m_variant = variant;
        return groupData;
    }

    void TropicalFish::SaveToBucket(ItemStack& bucket) const {
        // MC TropicalFish.saveToBucketTag: super (the default keys), then
        // bucket.copyFrom(TROPICAL_FISH_PATTERN / _BASE_COLOR /
        // _PATTERN_COLOR, this) — the entity's get() answers each from the
        // packed variant.
        SchoolingFish::SaveToBucket(bucket);
        bucket.components.set(DataComponents::TROPICAL_FISH_PATTERN,
                              static_cast<int32_t>(m_variant.pattern));
        bucket.components.set(DataComponents::TROPICAL_FISH_BASE_COLOR,
                              static_cast<int32_t>(m_variant.baseColor));
        bucket.components.set(DataComponents::TROPICAL_FISH_PATTERN_COLOR,
                              static_cast<int32_t>(m_variant.patternColor));
    }

    void TropicalFish::ApplyImplicitComponents(const ItemStack& stack) {
        // MC TropicalFish.applyImplicitComponents: each component present
        // replaces its third (setPattern / setBaseColor / setPatternColor
        // repack around the other two).
        if (auto p = stack.components.get(DataComponents::TROPICAL_FISH_PATTERN);
            p && *p >= 0 && *p < TropicalFishVariants::kPatternCount) {
            m_variant.pattern = static_cast<TropicalFishVariants::Pattern>(*p);
        }
        if (auto c = stack.components.get(DataComponents::TROPICAL_FISH_BASE_COLOR)) {
            m_variant.baseColor = TropicalFishVariants::DyeById(*c);
        }
        if (auto c = stack.components.get(DataComponents::TROPICAL_FISH_PATTERN_COLOR)) {
            m_variant.patternColor = TropicalFishVariants::DyeById(*c);
        }
    }

    // ── Salmon (MC Salmon) ─────────────────────────────────────────────────

    const char* Salmon::SizeName(uint8_t size) {
        switch (size) {
            case kSmall: return "small";
            case kLarge: return "large";
            default:     return "medium";
        }
    }

    int Salmon::SizeFromName(const std::string& name) {
        std::string_view n = name;
        if (n.rfind("minecraft:", 0) == 0) n.remove_prefix(10);
        if (n == "small")  return kSmall;
        if (n == "medium") return kMedium;
        if (n == "large")  return kLarge;
        return -1;
    }

    std::shared_ptr<SpawnGroupData>
    Salmon::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        // MC Salmon.finalizeSpawn: WeightedList{SMALL 30, MEDIUM 50,
        // LARGE 15}.getRandom — nextInt(total) walked in insertion order —
        // BEFORE super.
        if (m_level) {
            const int roll = m_level->Random().NextInt(30 + 50 + 15);
            SetSize(roll < 30 ? kSmall : (roll < 80 ? kMedium : kLarge));
        }
        return SchoolingFish::FinalizeSpawn(reason, std::move(groupData));
    }

    void Salmon::SaveToBucket(ItemStack& bucket) const {
        // MC Salmon.saveToBucketTag: the default keys + copyFrom(SALMON_SIZE).
        Bucketable::SaveDefaultDataToBucketTag(*this, bucket);
        bucket.components.set(DataComponents::SALMON_SIZE, static_cast<int32_t>(m_size));
    }

    void Salmon::ApplyImplicitComponents(const ItemStack& stack) {
        if (auto size = stack.components.get(DataComponents::SALMON_SIZE)) SetSize(*size);
    }

    // ── Nautilus / ZombieNautilus ──────────────────────────────────────────

    AbstractNautilus::AbstractNautilus(EntityTypeId type, EntityLevel* level)
        : GenericAnimal(type, level), TamableAnimal(this) {
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

    // ── Taming and feeding ─────────────────────────────────────────────────

    bool AbstractNautilus::IsTamingItem(uint32_t itemId) {
        // ItemTags.NAUTILUS_TAMING_ITEMS.
        return itemId == Items::Pufferfish || itemId == Items::PufferfishBucket;
    }

    bool AbstractNautilus::IsBucketFood(uint32_t itemId) {
        // ItemTags.NAUTILUS_BUCKET_FOOD.
        return itemId == Items::PufferfishBucket || itemId == Items::CodBucket ||
               itemId == Items::SalmonBucket || itemId == Items::TropicalFishBucket;
    }

    bool AbstractNautilus::IsNautilusFood(uint32_t itemId) {
        // ItemTags.NAUTILUS_FOOD = #fishes + #nautilus_bucket_food.
        return itemId == Items::Cod || itemId == Items::CookedCod || itemId == Items::Salmon ||
               itemId == Items::CookedSalmon || itemId == Items::Pufferfish ||
               itemId == Items::TropicalFish || IsBucketFood(itemId);
    }

    bool AbstractNautilus::IsFood(uint32_t itemId) const {
        // MC isFood: `!isTame() && !isBaby() ? NAUTILUS_TAMING_ITEMS : NAUTILUS_FOOD`.
        return !IsTame() && !IsBaby() ? IsTamingItem(itemId) : IsNautilusFood(itemId);
    }

    void AbstractNautilus::OpenCustomInventoryScreen(LivingEntity& player) {
        // MC AbstractNautilus.openCustomInventoryScreen: !isClientSide,
        // (!isVehicle || hasPassenger(player)), isTame.
        if (!m_level || m_level->IsClientSide()) return;
        if ((!IsVehicle() || HasPassenger(player)) && IsTame()) m_level->OpenMountInventory(player, *this);
    }

    UseResult AbstractNautilus::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC AbstractNautilus.interact: any click makes it persistent.
        const bool clientSide = m_level && m_level->IsClientSide();
        if (!clientSide) SetPersistenceRequired(true);

        // MC usePlayerItem for the super path: a bucket food is eaten out of
        // the bucket, which comes back as water (Animal's plain shrink is
        // undone and refilled — the Axolotl pattern).
        const auto animalInteract = [&]() {
            const ItemStack before = held;
            const UseResult result = GenericAnimal::MobInteract(player, held);
            const bool spent = held.itemId != before.itemId || held.count < before.count;
            if (spent && IsBucketFood(before.itemId) && !clientSide) {
                held = before;
                m_level->CreateFilledResult(player, held, ItemStack(Items::WaterBucket, 1));
            }
            return result;
        };

        if (IsBaby()) return animalInteract();

        const bool sneaking = m_level && m_level->IsPlayerSneaking(player);
        if (IsTame() && sneaking) {
            OpenCustomInventoryScreen(player);
            return UseResult::Success;
        }

        if (!held.IsEmpty()) {
            if (!clientSide && !IsTame() && IsFood(held.itemId)) {
                // MC usePlayerItem, then tryToTame.
                if (IsBucketFood(held.itemId)) {
                    m_level->CreateFilledResult(player, held, ItemStack(Items::WaterBucket, 1));
                } else {
                    UsePlayerItem(held);
                }
                TryToTame(player);
                return UseResult::SuccessServer;
            }

            if (IsFood(held.itemId) && GetHealth() < GetMaxHealth()) {
                // MC feed(player, hand, stack, 2.0F, 1.0F): usePlayerItem (the
                // bucket refill), heal by nutrition x 2 (a bucket has no FOOD
                // component: the default 1.0), the chew.
                if (!clientSide) {
                    if (IsBucketFood(held.itemId)) {
                        m_level->CreateFilledResult(player, held, ItemStack(Items::WaterBucket, 1));
                        Heal(1.0f);
                        PlayEatingSound();
                    } else {
                        Feed(held, 2.0f, 1.0f);
                    }
                }
                return UseResult::Success;
            }

            if (const auto interact = ItemRegistry::Get(held.itemId).interactLivingEntity) {
                if (!clientSide) {
                    const UseResult r = interact(held, *this);
                    if (ConsumesAction(r)) return r;
                }
            }
        }

        if (IsTame() && !sneaking && !IsFood(held.itemId)) {
            DoPlayerRide(player);
            return UseResult::Success;
        }
        return animalInteract();
    }

    void AbstractNautilus::TryToTame(LivingEntity& player) {
        // MC tryToTame: 1 in 3 — tame, stop, the hearts; else the smoke.
        // Either way the chew.
        if (!m_level) return;
        if (m_level->Random().NextInt(3) == 0) {
            Tame(player);
            GetNavigation().Stop();
            m_level->BroadcastEntityEvent(*this, 7);
        } else {
            m_level->BroadcastEntityEvent(*this, 6);
        }
        PlayEatingSound();
    }

    void AbstractNautilus::DoPlayerRide(LivingEntity& player) {
        // MC doPlayerRide: server only; `if (!isVehicle()) clearHome()` after
        // the attempt.
        if (!m_level || m_level->IsClientSide()) return;
        m_level->StartPlayerRiding(player, *this);
        if (!IsVehicle()) ClearHome();
    }

    // ── Riding ─────────────────────────────────────────────────────────────

    bool AbstractNautilus::CanBeSteeredBy(const RiderControl& rider) const {
        // MC getControllingPassenger: `isSaddled() && getFirstPassenger()
        // instanceof Player` — the resolver only answers for a player.
        (void)rider;
        return IsSaddled();
    }

    glm::dvec3 AbstractNautilus::GetRiddenInput(const RiderControl& rider, const glm::dvec3& selfInput) {
        // MC getRiddenInput: strafe as pressed; forward/back follows the
        // rider's pitch (backwards at half), so looking down dives.
        (void)selfInput;
        const float strafe = rider.xxa;
        float forward = 0.0f;
        float up = 0.0f;
        if (rider.zza != 0.0f) {
            float forwardLook = std::cos(rider.xRot * Mth::kDegToRad);
            float upLook = -std::sin(rider.xRot * Mth::kDegToRad);
            if (rider.zza < 0.0f) {
                forwardLook *= -0.5f;
                upLook *= -0.5f;
            }
            up = upLook;
            forward = forwardLook;
        }
        return glm::dvec3(strafe, up, forward);
    }

    void AbstractNautilus::TickRidden(const RiderControl& rider, const glm::dvec3& riddenInput) {
        // MC tickRidden: the shell turns half-way to the rider's yaw each
        // tick and pitches at half the rider's.
        (void)riddenInput;
        const float targetXRot = rider.xRot * 0.5f;
        const float diff = Mth::WrapDegrees(rider.yRot - yRot);
        const float newYRot = yRot + diff * 0.5f;
        // setRot: `% 360` on both; the O/body/head copies take the local
        // (unreduced) yaw, as MC's chained assignment does.
        yRot = std::fmod(newYRot, 360.0f);
        xRot = std::fmod(targetXRot, 360.0f);
        yRotO = newYRot;
        yBodyRot = newYRot;
        yHeadRot = newYRot;
        if (CanSimulateMountMovement()) {
            if (m_playerJumpPendingScale > 0.0f && !jumping) {
                ExecuteRidersJump(m_playerJumpPendingScale, rider);
            }
            m_playerJumpPendingScale = 0.0f;
        }
    }

    float AbstractNautilus::GetRiddenSpeed(const RiderControl& rider) const {
        (void)rider;
        const float speed = static_cast<float>(GetAttributeValue(Attribute::MovementSpeed));
        return IsInWater() ? kRiddenSpeedInWater * speed : kRiddenSpeedOnLand * speed;
    }

    void AbstractNautilus::Travel(const glm::dvec3& input) {
        // MC travel → shouldTravelInFluid → travelInFluid → the travelInWater
        // override: moveRelative(getSpeed()), move, x0.9 — no gravity, no
        // slow-down table, no jump-out. (floatInLiquidWhileRidden is for
        // #can_float_while_ridden, which the nautilus is not.) Lava and air
        // are the base travel.
        if (IsInWater() && IsAffectedByFluids()) {
            MoveRelative(GetSpeed(), input);
            Move(velocity);
            velocity *= 0.9;
            return;
        }
        GenericAnimal::Travel(input);
    }

    glm::dvec3 AbstractNautilus::GetPassengerAttachmentPoint(const Entity& passenger) const {
        // EntityTypes NAUTILUS / ZOMBIE_NAUTILUS .passengerAttachments(1.1375F);
        // Nautilus.BABY_DIMENSIONS attach PASSENGER at (0, 0.5, 0). A y-only
        // point: the yaw rotation leaves it as is.
        (void)passenger;
        return glm::dvec3(0.0, IsBaby() ? 0.5 : 1.1375, 0.0);
    }

    float AbstractNautilus::BlockSpeedFactor() const {
        // MC LivingEntity.getBlockSpeedFactor: MOVEMENT_EFFICIENCY lerps the
        // block's factor toward 1; Entity.getBlockSpeedFactor reads the block
        // here (water and bubble columns answer for themselves), else below.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        float factor = 1.0f;
        if (blocks) {
            const auto factorOf = [](BlockID id) {
                return (id == BlockID::SoulSand || id == BlockID::HoneyBlock) ? 0.4f : 1.0f;
            };
            const glm::ivec3 p = BlockPosition();
            const BlockID here = blocks->GetBlock(p.x, p.y, p.z);
            factor = factorOf(here);
            if (here != BlockID::Water && here != BlockID::BubbleColumn && factor == 1.0f) {
                const int belowY = static_cast<int>(std::floor(position.y - 0.500001));
                factor = factorOf(blocks->GetBlock(p.x, belowY, p.z));
            }
        }
        const float efficiency = static_cast<float>(GetAttributeValue(Attribute::MovementEfficiency));
        return efficiency >= 1.0f ? 1.0f : Mth::Lerp(efficiency, factor, 1.0f);
    }

    void AbstractNautilus::ExecuteRidersJump(float amount, const RiderControl& rider) {
        // MC executeRidersJump: a push along the rider's look (1.2 in water,
        // 0.5 on land, x the charge x MOVEMENT_SPEED x the block factor), the
        // 40-tick cooldown and the DASH flag.
        const glm::vec3 look = Mth::ViewVector(rider.xRot, rider.yRot);
        const double scale = static_cast<double>((IsInWater() ? kDashMomentumInWater : kDashMomentumOnLand) * amount) *
                             GetAttributeValue(Attribute::MovementSpeed) *
                             static_cast<double>(BlockSpeedFactor());
        velocity += glm::dvec3(look) * scale;
        m_dashCooldown = kDashCooldownTicks;
        SetDashing(true);
        needsSync = true;
    }

    void AbstractNautilus::OnPlayerJump(int jumpAmount) {
        // MC onPlayerJump: charge only while saddled and off cooldown;
        // PlayerRideableJumping.getPlayerJumpPendingScale.
        if (!IsSaddled() || m_dashCooldown > 0) return;
        m_playerJumpPendingScale = jumpAmount >= 90 ? 1.0f
                                                    : 0.4f + 0.4f * static_cast<float>(jumpAmount) / 90.0f;
    }

    void AbstractNautilus::HandleStartJump(int jumpScale) {
        // MC handleStartJump (server): the dash sound, ENTITY_ACTION, DASH.
        (void)jumpScale;
        MakeSound(GetDashSound());
        GameEvent(GameEventId::EntityAction);
        SetDashing(true);
    }

    void AbstractNautilus::SetDashing(bool dashing) {
        if (dashing == m_dashing) return;
        m_dashing = dashing;
        // MC onSyncedDataUpdated(DASH), `!firstTick`: a change arms the
        // cooldown when none runs (the server's own set and a client's
        // synched copy alike).
        if (!firstTick) m_dashCooldown = m_dashCooldown == 0 ? kDashCooldownTicks : m_dashCooldown;
    }

    void AbstractNautilus::ApplyEffects() {
        // MC applyEffects: a player in the first seat breathes under water —
        // BREATH_OF_THE_NAUTILUS for 60 ticks, ambient, refreshed every 40.
        LivingEntity* player = PlayerRideable::FirstPlayerPassenger(*this);
        if (!player || !m_level) return;
        const bool hasEffect = player->HasEffect(MobEffectId::BreathOfTheNautilus);
        const bool shouldRefresh = m_level->GetGameTime() % kEffectRefreshRate == 0;
        if (!hasEffect || shouldRefresh) {
            player->AddEffect(MobEffectInstance(MobEffectId::BreathOfTheNautilus, kEffectDuration, 0,
                                                true, true, true));
        }
    }

    void AbstractNautilus::SpawnBubbles() {
        // MC spawnBubbles: faster swimming, denser trail, out of the back of
        // the shell. (Client-visible only; the server's AddParticle is a no-op.)
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();
        const double speed = glm::length(velocity);
        const double bubbleProbability = std::clamp(speed * 2.0, 0.15000000596046448, 1.0);
        if (static_cast<double>(rng.NextFloat()) < bubbleProbability) {
            const float xr = std::clamp(xRot, -10.0f, 10.0f);
            const glm::vec3 mouth = Mth::ViewVector(xr, yRot);
            const double spread = rng.NextDouble() * 0.8 * (1.0 + speed);
            const double dx = (static_cast<double>(rng.NextFloat()) - 0.5) * spread;
            const double dy = (static_cast<double>(rng.NextFloat()) - 0.5) * spread;
            const double dz = (static_cast<double>(rng.NextFloat()) - 0.5) * spread;
            m_level->AddParticle(ParticleKind::Bubble,
                                 position.x - static_cast<double>(mouth.x) * 1.1,
                                 position.y - static_cast<double>(mouth.y) + 0.25,
                                 position.z - static_cast<double>(mouth.z) * 1.1, dx, dy, dz);
        }
    }

    void AbstractNautilus::Tick() {
        GenericAnimal::Tick();
        if (!m_level) return;
        if (!m_level->IsClientSide()) ApplyEffects();

        if (IsDashing() && m_dashCooldown < kDashCooldownTicks - kDashMinimumDuration) SetDashing(false);

        if (m_dashCooldown > 0) {
            --m_dashCooldown;
            if (m_dashCooldown == 0) MakeSound(GetDashReadySound());
        }

        if (IsInWater() && m_level->IsClientSide()) SpawnBubbles();
    }

    void AbstractNautilus::CheckRestriction() {
        // MC checkRestriction: a tame nautilus off the lead and without a
        // rider keeps a home around itself — 32 blocks for an unsaddled adult,
        // 16 otherwise — re-centred once it strays past radius + 8.
        if (IsLeashed() || IsVehicle() || !IsTame()) return;
        const int radius = !IsBaby() && GetEquipment(EquipmentSlot::SADDLE).IsEmpty()
                               ? kLargeRestrictionRadius : kSmallRestrictionRadius;
        const glm::ivec3 here = BlockPosition();
        bool recentre = !HasHome() || radius != GetHomeRadius();
        if (!recentre) {
            const glm::ivec3 d = GetHomePosition() - here;
            const double distSqr = static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y +
                                   static_cast<double>(d.z) * d.z;
            const double limit = static_cast<double>(radius + kRestrictionRadiusBuffer);
            recentre = !(distSqr < limit * limit);
        }
        if (recentre) SetHomeTo(here, radius);
    }

    void AbstractNautilus::CustomServerAiStep() {
        CheckRestriction();
        GenericAnimal::CustomServerAiStep();
    }

    Nautilus::Nautilus(EntityLevel* level)
        : AbstractNautilus(EntityTypeId::Nautilus, level) {
        m_brain = std::make_unique<Brain>();
        NautilusAi::InitBrain(*this, *m_brain);
    }

    void Nautilus::UpdateBrainActivity() { NautilusAi::UpdateActivity(*this); }

    std::unique_ptr<Animal> Nautilus::CreateBaby() {
        // MC Nautilus.getBreedOffspring: `if (isTame()) { setOwnerReference;
        // setTame(true, true); }`.
        std::unique_ptr<Animal> baby = GenericAnimal::CreateBaby();
        if (baby && IsTame()) {
            if (auto* foal = dynamic_cast<Nautilus*>(baby.get())) {
                foal->SetOwnerUuid(GetOwnerUuid());
                foal->SetTame(true, true);
            }
        }
        return baby;
    }

    const char* Nautilus::GetAmbientSound() const {
        if (IsBaby()) {
            return IsUnderWater() ? SoundEvents::BABY_NAUTILUS_AMBIENT : SoundEvents::BABY_NAUTILUS_AMBIENT_ON_LAND;
        }
        return IsUnderWater() ? SoundEvents::NAUTILUS_AMBIENT : SoundEvents::NAUTILUS_AMBIENT_ON_LAND;
    }

    const char* Nautilus::GetHurtSound(MobDamageSource source) const {
        (void)source;
        if (IsBaby()) {
            return IsUnderWater() ? SoundEvents::BABY_NAUTILUS_HURT : SoundEvents::BABY_NAUTILUS_HURT_ON_LAND;
        }
        return IsUnderWater() ? SoundEvents::NAUTILUS_HURT : SoundEvents::NAUTILUS_HURT_ON_LAND;
    }

    const char* Nautilus::GetDeathSound() const {
        if (IsBaby()) {
            return IsUnderWater() ? SoundEvents::BABY_NAUTILUS_DEATH : SoundEvents::BABY_NAUTILUS_DEATH_ON_LAND;
        }
        return IsUnderWater() ? SoundEvents::NAUTILUS_DEATH : SoundEvents::NAUTILUS_DEATH_ON_LAND;
    }

    ZombieNautilus::ZombieNautilus(EntityLevel* level)
        : AbstractNautilus(EntityTypeId::ZombieNautilus, level) {
        m_brain = std::make_unique<Brain>();
        ZombieNautilusAi::InitBrain(*this, *m_brain);
    }

    void ZombieNautilus::UpdateBrainActivity() {
        ZombieNautilusAi::UpdateActivity(*this);
    }

} // namespace Game
