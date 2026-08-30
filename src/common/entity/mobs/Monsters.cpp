// File: src/common/entity/mobs/Monsters.cpp
#include "common/entity/mobs/Monsters.hpp"
#include "common/world/level/Explosion.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/RangedGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/goals/MoveToBlockGoal.hpp"
#include "common/entity/ai/goals/EndermanGoals.hpp"
#include "common/entity/ai/goals/RestrictionGoals.hpp"
#include "common/entity/ai/goals/GuardianGoals.hpp"
#include "common/entity/ai/goals/PhantomGoals.hpp"
#include "common/entity/ai/goals/EvokerGoals.hpp"
#include "common/entity/ai/goals/IllusionerGoals.hpp"
#include "common/entity/ai/goals/DoorGoals.hpp"
#include "common/entity/ai/goals/WitherGoals.hpp"
#include "common/entity/ai/goals/StriderGoals.hpp"
#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/navigation/FlyingPathNavigation.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/projectile/HurtingProjectile.hpp"
#include "common/entity/projectile/EvokerFangs.hpp"
#include "common/entity/projectile/AreaEffectCloud.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/projectile/Arrow.hpp"
#include "common/entity/projectile/ThrowableProjectile.hpp"
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/ai/navigation/WaterBoundPathNavigation.hpp"
#include "common/world/spawn/SpawnPlacements.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/physics/Physics.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Game {

    // ── Zombie ─────────────────────────────────────────────────────────────

    void Zombie::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::FollowRange,   35.0);
        out.Register(Attribute::MovementSpeed,  0.23);
        out.Register(Attribute::AttackDamage,   3.0);
        out.Register(Attribute::Armor,          2.0);
        out.Register(Attribute::SpawnReinforcements, 0.0);
    }

    Zombie::Zombie(EntityLevel* level) : Zombie(EntityTypeId::Zombie, level) {
        RegisterGoals();
    }

    Zombie::Zombie(EntityTypeId type, EntityLevel* level) : Monster(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // RegisterGoals is deliberately NOT called here: a virtual dispatched
        // during base construction binds to the base's override, so each
        // concrete class calls it from its own constructor (the file-wide
        // rule Mob.hpp states).
    }

    // ── Zombie variants ────────────────────────────────────────────────────

    Drowned::Drowned(EntityLevel* level) : Zombie(EntityTypeId::Drowned, level) {
        // MC Drowned: water costs nothing to path through, and the amphibious
        // navigation lets its paths dive and surface.
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetNavigation(std::make_unique<AmphibiousPathNavigation>(this, level));
        RegisterGoals();
    }

    void Drowned::AddBehaviourGoals() {
        // MC Drowned.addBehaviourGoals puts the trident goal at priority 2
        // alongside its melee (DrownedAttackGoal); the zombie set below
        // supplies the melee at MC-Zombie priorities. The goal's canUse gate
        // (holding a trident) makes the two mutually exclusive per drowned.
        m_goalSelector.AddGoal(2, std::make_unique<DrownedTridentAttackGoal>(
                                      this, 1.0, 40, 10.0f));
        Zombie::AddBehaviourGoals();
    }

    std::shared_ptr<SpawnGroupData>
    Drowned::FinalizeSpawn(SpawnReason reason,
                           std::shared_ptr<SpawnGroupData> groupData) {
        auto data = Zombie::FinalizeSpawn(reason, std::move(groupData));
        // MC Drowned.populateDefaultEquipmentSlots: 10% roll a held weapon,
        // and 10-in-16 of those a trident — 6.25% overall. The same two draws
        // in the same order, minus the fishing rod (no mob equipment system;
        // the flag IS the trident).
        if (m_level && m_level->Random().NextFloat() > 0.9f) {
            m_hasTrident = m_level->Random().NextInt(16) < 10;
        }
        return data;
    }

    void Drowned::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_level) return;

        auto trident = std::make_unique<ThrownTrident>(m_level);
        trident->SetOwner(this);
        trident->position = glm::dvec3(position.x, GetEyeY() - 0.1, position.z);

        // MC: aim a third of the way up the target's box with the standard
        // 0.2-per-horizontal-block loft.
        const double xd = target.position.x - position.x;
        const double yd = (target.position.y + target.GetBbHeight() / 3.0) -
                          trident->position.y;
        const double zd = target.position.z - position.z;
        const double horiz = std::sqrt(xd * xd + zd * zd);

        const int difficultyId = static_cast<int>(m_level->GetDifficulty());
        trident->Shoot(xd, yd + horiz * 0.2, zd, 1.6f,
                       static_cast<float>(14 - difficultyId * 4));

        m_level->AddFreshEntity(std::move(trident));
    }

    bool Husk::DoHurtTarget(Entity& target) {
        // MC Husk.doHurtTarget: super first, then HUNGER on a landed hit.
        const bool result = Zombie::DoHurtTarget(target);
        if (result) {
            // MC gates on getMainHandItem().isEmpty() — a husk holding a
            // weapon does not sting. No mob equipment system exists, so every
            // husk's hand is empty, which is also MC's common case.
            if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
                // MC: 140 * (int)effectiveDifficulty ticks of HUNGER I.
                // Regional difficulty (chunk inhabited time, moon) is not
                // modelled; these are the fresh-world base values —
                // EASY 0.75, NORMAL 1.5, HARD 2.25 — truncated: 0 / 1 / 2.
                int effectiveDifficulty = 0;
                if (m_level) {
                    switch (m_level->GetDifficulty()) {
                        case Difficulty::Normal: effectiveDifficulty = 1; break;
                        case Difficulty::Hard:   effectiveDifficulty = 2; break;
                        default: break;
                    }
                }
                if (effectiveDifficulty > 0) {
                    living->AddEffect(
                        MobEffectInstance(MobEffectId::Hunger,
                                          140 * effectiveDifficulty),
                        this);
                }
            }
        }
        return result;
    }

    // ── In-water conversion (MC Zombie.tick / convertToZombieType) ─────────

    void Zombie::Tick() {
        // MC Zombie.tick: the conversion clock runs server-side, before
        // super.tick(), only while alive and AI'd.
        if (m_level && !m_level->IsClientSide() && IsAlive() && !IsNoAi()) {
            if (m_underWaterConverting) {
                --m_conversionTime;
                if (m_conversionTime < 0) {
                    DoUnderWaterConversion();
                }
            } else if (ConvertsInWater()) {
                if (IsEyeInWater()) {
                    ++m_inWaterTime;
                    if (m_inWaterTime >= 600) {
                        StartUnderWaterConversion(300);
                    }
                } else {
                    m_inWaterTime = -1;
                }
            }
        }
        Monster::Tick();
    }

    void Zombie::DoUnderWaterConversion() {
        // MC Zombie.doUnderWaterConversion: → DROWNED. Level event 1040 (the
        // conversion gurgle) waits on the sound system.
        ConvertToZombieType(std::make_unique<Drowned>(m_level));
    }

    void Zombie::ConvertToZombieType(std::unique_ptr<Zombie> replacement) {
        // MC Zombie.convertToZombieType = convertTo(type, SINGLE(keepEquipment,
        // preserveCanPickUpLoot), afterConversion: handleAttributes(...,
        // CONVERSION)). The generic copy, the zombie-family copies
        // (convertCommon's setBaby + the canBreakDoors carry), then MC's
        // afterConversion callback — which needs the position already copied,
        // hence the ordering.
        if (IsRemoved() || !replacement || !m_level) return;

        CopyConversionState(*replacement);
        replacement->SetBaby(IsBaby());
        replacement->SetCanBreakDoors(CanBreakDoors());
        replacement->HandleAttributes(
            GetSpecialMultiplier(m_level->GetDifficulty()),
            SpawnReason::Conversion);
        FinishConversion(std::move(replacement));
    }

    void Husk::DoUnderWaterConversion() {
        // MC Husk.doUnderWaterConversion: → ZOMBIE (which then runs its own
        // clock into a drowned). Level event 1041 waits on the sound system.
        ConvertToZombieType(std::make_unique<Zombie>(m_level));
    }

    // ── ZombifiedPiglin ────────────────────────────────────────────────────

    ZombifiedPiglin::ZombifiedPiglin(EntityLevel* level)
        : Zombie(EntityTypeId::ZombifiedPiglin, level), NeutralMob(this) {
        // MC: lava is merely expensive, not forbidden — it lives in the nether.
        SetPathfindingMalus(PathType::Lava, 8.0f);
        RegisterGoals();
    }

    void ZombifiedPiglin::AddBehaviourGoals() {
        // MC ZombifiedPiglin.addBehaviourGoals, priority for priority
        // (SpearUseGoal(1) omitted — no spear item, matching the zombie's
        // omission).
        m_goalSelector.AddGoal(2, std::make_unique<ZombieAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));

        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        // MC: NearestAttackableTargetGoal(Player, 10, true, false,
        // this::isAngryAt) — the piglin only hunts players it is ANGRY at.
        auto angryAt = std::make_unique<NearestAttackableTargetGoal>(
            this, /*mustSee=*/true, /*mustReach=*/false, /*randomInterval=*/10);
        angryAt->SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<ZombifiedPiglin&>(mob).IsAngryAt(target);
        });
        m_targetSelector.AddGoal(2, std::move(angryAt));
        m_targetSelector.AddGoal(3, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/true));
    }

    void ZombifiedPiglin::SetTarget(LivingEntity* target) {
        // MC ZombifiedPiglin.setTarget: a target where there was none arms
        // the alert clock. (FIRST_ANGER_SOUND_DELAY and the angry sound wait
        // on the sound system.)
        if (GetTarget() == nullptr && target != nullptr && m_level) {
            m_ticksUntilNextAlert = 80 + m_level->Random().NextInt(41);  // ALERT_INTERVAL 4..6 s
        }
        Zombie::SetTarget(target);
    }

    void ZombifiedPiglin::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39):
        // uniform 400..780 ticks.
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void ZombifiedPiglin::CustomServerAiStep() {
        // MC ZombifiedPiglin.customServerAiStep, step for step: the angry
        // speed modifier (adults only), the anger update, the herd alert.
        if (IsAngry()) {
            if (!IsBaby() && !m_attributes.HasModifier(Attribute::MovementSpeed,
                                                       ModifierId::PiglinAttackingSpeed)) {
                m_attributes.AddModifier(Attribute::MovementSpeed,
                    AttributeModifier{
                        static_cast<uint32_t>(ModifierId::PiglinAttackingSpeed),
                        0.05, AttributeOperation::AddValue });
            }
            // maybePlayFirstAngerSound() — sounds wait on the sound system.
        } else if (m_attributes.HasModifier(Attribute::MovementSpeed,
                                            ModifierId::PiglinAttackingSpeed)) {
            m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                        ModifierId::PiglinAttackingSpeed);
        }

        UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
        if (GetTarget() != nullptr) {
            MaybeAlertOthers();
        }

        Zombie::CustomServerAiStep();
    }

    void ZombifiedPiglin::MaybeAlertOthers() {
        // MC maybeAlertOthers: the clock ticks down; on zero, alert (if the
        // target is in sight) and re-arm.
        if (m_ticksUntilNextAlert > 0) {
            --m_ticksUntilNextAlert;
            return;
        }
        if (GetTarget() && GetSensing().HasLineOfSight(*GetTarget())) {
            AlertOthers();
        }
        m_ticksUntilNextAlert = 80 + m_level->Random().NextInt(41);
    }

    void ZombifiedPiglin::AlertOthers() {
        // MC alertOthers: every idle zombified piglin in the follow-range
        // box (±10 vertically) takes this piglin's target. (MC also skips
        // piglins ALLIED to the target — scoreboard teams, none here.)
        if (!m_level || !GetTarget()) return;
        const double within = GetAttributeValue(Attribute::FollowRange);
        AABB box;
        box.min = glm::vec3(position);
        box.max = box.min + glm::vec3(1.0f);
        box.min -= glm::vec3(within, 10.0f, within);
        box.max += glm::vec3(within, 10.0f, within);

        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);
        for (Entity* e : nearby) {
            if (e->GetType() != EntityTypeId::ZombifiedPiglin) continue;
            auto* other = static_cast<ZombifiedPiglin*>(e);
            if (other->GetTarget() != nullptr) continue;
            other->SetTarget(GetTarget());
        }
    }

    void Zombie::SetBaby(bool baby) {
        if (m_baby == baby) return;
        m_baby = baby;

        // MC SPEED_MODIFIER_BABY: +50% movement speed, as a multiplier on the
        // base rather than a flat add, so it scales with any other modifier.
        if (baby) {
            m_attributes.AddModifier(Attribute::MovementSpeed,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::BabySpeedBoost), 0.5,
                                   AttributeOperation::AddMultipliedBase });
        } else {
            m_attributes.RemoveModifier(Attribute::MovementSpeed, ModifierId::BabySpeedBoost);
        }
    }

    void Zombie::SetCanBreakDoors(bool v) {
        // MC Zombie.setCanBreakDoors — gated on navigation.canNavigateGround()
        // in MC; every zombie family member here walks, so the guard is a
        // comment. Toggling on registers the goal at priority 1 and lets the
        // pathfinder route THROUGH closed wooden doors; toggling off removes
        // both.
        if (m_canBreakDoors == v) return;
        m_canBreakDoors = v;
        GetNavigation().SetCanOpenDoors(v);
        if (v) {
            auto goal =
                std::make_unique<BreakDoorGoal>(this, &BreakDoorGoal::HardOnly);
            m_breakDoorGoal = goal.get();
            m_goalSelector.AddGoal(1, std::move(goal));
        } else if (m_breakDoorGoal) {
            m_goalSelector.RemoveGoal(m_breakDoorGoal);
            m_breakDoorGoal = nullptr;
        }
    }

    namespace {
        // Target/avoid type lists. File-scope because the goals keep pointers.
        constexpr EntityTypeId kVillagerTargets[]  = { EntityTypeId::Villager,
                                                       EntityTypeId::WanderingTrader };
        constexpr EntityTypeId kIronGolemTargets[] = { EntityTypeId::IronGolem };
        constexpr EntityTypeId kTurtleTargets[]    = { EntityTypeId::Turtle };
        constexpr EntityTypeId kWolfAvoid[]        = { EntityTypeId::Wolf };
        constexpr EntityTypeId kCatAvoid[]         = { EntityTypeId::Cat };
        constexpr EntityTypeId kOcelotAvoid[]      = { EntityTypeId::Ocelot };
        constexpr EntityTypeId kArmadilloAvoid[]   = { EntityTypeId::Armadillo };
    }

    void Zombie::RegisterGoals() {
        // MC Zombie.registerGoals. SpearUseGoal(2) omitted — no spear item.
        // Everything else is priority-for-priority.
        m_goalSelector.AddGoal(4, std::make_unique<ZombieAttackTurtleEggGoal>(this, 1.0, 3));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        AddBehaviourGoals();
    }

    void Zombie::AddBehaviourGoals() {
        // MC addBehaviourGoals. MoveThroughVillageGoal(6) omitted — no
        // villages or doors for it to path between.
        m_goalSelector.AddGoal(3, std::make_unique<ZombieAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));

        // Alerting others is what turns hitting one zombie into a group fight.
        // MC Zombie.java:117: setAlertOthers(ZombifiedPiglin.class) — the box
        // gathers getEntitiesOfClass(mob.getClass()), i.e. the RUNTIME class:
        // a plain zombie's scan matches the whole Zombie family (husks,
        // drowned, zombie villagers are subclasses), while a husk running
        // this same registration matches only husks. The argument lists the
        // classes to SKIP, so zombified piglins hold their neutrality either
        // way.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers([](const Mob& self, const Mob& other) {
            if (other.GetType() == EntityTypeId::ZombifiedPiglin) return false;
            if (self.GetType() == EntityTypeId::Zombie) {
                return dynamic_cast<const Zombie*>(&other) != nullptr;
            }
            return other.GetType() == self.GetType();
        });
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, true));
        // MC: AbstractVillager at 3 with mustSee=false, IronGolem at 3 with
        // mustSee=true.
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kVillagerTargets, 2, false));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kIronGolemTargets, 1, true));
        // MC Zombie.java:121 — Turtle at 5, mustSee, with
        // Turtle.BABY_ON_LAND_SELECTOR: only a BABY turtle OUT of the water.
        auto turtleGoal = std::make_unique<NearestAttackableTargetGoal>(
            this, kTurtleTargets, 1, /*mustSee=*/true);
        turtleGoal->SetSelector([](Mob&, const LivingEntity& t) {
            return t.IsBaby() && !t.IsInWater();
        });
        m_targetSelector.AddGoal(5, std::move(turtleGoal));
    }

    std::shared_ptr<SpawnGroupData>
    Zombie::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Monster::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level) return groupData;

        JavaRandom& rng = m_level->Random();
        const float difficultyModifier = GetSpecialMultiplier(m_level->GetDifficulty());

        if (reason != SpawnReason::Conversion) {
            SetCanPickUpLoot(rng.NextFloat() < 0.55f * difficultyModifier);
        }

        if (!groupData) {
            // getSpawnAsBabyOdds: 5%.
            groupData = std::make_shared<ZombieGroupData>(rng.NextFloat() < 0.05f, true);
        }

        if (auto* zombieData = dynamic_cast<ZombieGroupData*>(groupData.get())) {
            if (zombieData->isBaby) {
                SetBaby(true);
                if (zombieData->canSpawnJockey) {
                    // MC's two 5% chicken-jockey rolls, in MC's else-if
                    // shape (the second is only drawn when the first fails).
                    if (rng.NextFloat() < 0.05f) {
                        // MC: mount the FIRST unridden chicken in the box
                        // inflated (5, 3, 5) (ENTITY_NOT_BEING_RIDDEN).
                        AABB box = GetAABB();
                        box.min -= glm::vec3(5.0f, 3.0f, 5.0f);
                        box.max += glm::vec3(5.0f, 3.0f, 5.0f);
                        std::vector<Entity*> nearby;
                        m_level->GetEntitiesInBox(box, this, nearby);
                        for (Entity* e : nearby) {
                            if (e->GetType() != EntityTypeId::Chicken) continue;
                            if (!e->IsAlive() || e->IsVehicle()) continue;
                            // MC chicken.setChickenJockey(true): flips the
                            // despawn rule (a jockey chicken MAY despawn) and
                            // suppresses egg laying.
                            static_cast<Chicken*>(e)->SetChickenJockey(true);
                            StartRiding(*e, /*force=*/true);
                            break;
                        }
                    } else if (rng.NextFloat() < 0.05f) {
                        // MC: spawn a fresh chicken under it and ride.
                        auto chicken = std::make_unique<Chicken>(m_level);
                        chicken->position = position;
                        chicken->yRot = chicken->yBodyRot = chicken->yHeadRot = yRot;
                        chicken->FinalizeSpawn(SpawnReason::Jockey, nullptr);
                        Chicken* placed = chicken.get();
                        placed->SetChickenJockey(true);
                        m_level->AddFreshEntity(std::move(chicken));
                        StartRiding(*placed, /*force=*/true);
                    }
                }
            }

            SetCanBreakDoors(rng.NextFloat() < difficultyModifier * 0.1f);
            // populateDefaultEquipmentSlots / enchantments omitted — no mob
            // equipment system yet.
        }

        // Halloween pumpkin heads omitted with equipment.
        HandleAttributes(difficultyModifier, reason);
        return groupData;
    }

    void Zombie::RandomizeReinforcementsChance() {
        // MC Zombie.randomizeReinforcementsChance: base = nextDouble() * 0.1.
        m_attributes.SetBaseValue(Attribute::SpawnReinforcements,
                                  m_level->Random().NextDouble() * 0.1);
    }

    void ZombifiedPiglin::RandomizeReinforcementsChance() {
        // MC ZombifiedPiglin.randomizeReinforcementsChance: always 0.
        m_attributes.SetBaseValue(Attribute::SpawnReinforcements, 0.0);
    }

    void Zombie::HandleAttributes(float difficultyModifier, SpawnReason reason) {
        JavaRandom& rng = m_level->Random();

        RandomizeReinforcementsChance();

        m_attributes.RemoveModifier(Attribute::KnockbackResistance, ModifierId::RandomSpawnBonus);
        m_attributes.AddModifier(Attribute::KnockbackResistance,
            AttributeModifier{ static_cast<uint32_t>(ModifierId::RandomSpawnBonus),
                               rng.NextDouble() * 0.05, AttributeOperation::AddValue });

        // The zombie-specific follow-range bonus, ON TOP of Mob's universal
        // triangle roll — only applied when the roll exceeds 1.
        const double followRangeModifier = rng.NextDouble() * 1.5 * difficultyModifier;
        if (followRangeModifier > 1.0) {
            m_attributes.RemoveModifier(Attribute::FollowRange, ModifierId::ZombieRandomKnockback);
            m_attributes.AddModifier(Attribute::FollowRange,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::ZombieRandomKnockback),
                                   followRangeModifier, AttributeOperation::AddMultipliedTotal });
        }

        // The leader roll: 5% * difficulty. A leader calls reinforcements far
        // more often, has boosted max health, and can always break doors.
        if (rng.NextFloat() < difficultyModifier * 0.05f) {
            m_attributes.RemoveModifier(Attribute::SpawnReinforcements, ModifierId::ZombieLeaderReinf);
            m_attributes.AddModifier(Attribute::SpawnReinforcements,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::ZombieLeaderReinf),
                                   rng.NextDouble() * 0.25 + 0.5, AttributeOperation::AddValue });
            m_attributes.RemoveModifier(Attribute::MaxHealth, ModifierId::ZombieLeaderHealth);
            m_attributes.AddModifier(Attribute::MaxHealth,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::ZombieLeaderHealth),
                                   rng.NextDouble() * 3.0 + 1.0, AttributeOperation::AddMultipliedTotal });
            if (reason != SpawnReason::Conversion && reason != SpawnReason::Load) {
                m_health = GetMaxHealth();
            }
            SetCanBreakDoors(true);
        }
    }

    bool Zombie::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        if (!Monster::Hurt(source, amount, attacker)) return false;
        if (!m_level || m_level->IsClientSide()) return true;

        // MC Zombie.hurtServer's reinforcement call, HARD only.
        LivingEntity* target = GetTarget();
        if (!target) target = dynamic_cast<LivingEntity*>(attacker);
        if (!target) return true;
        if (m_level->GetDifficulty() != Difficulty::Hard) return true;

        JavaRandom& rng = m_level->Random();
        if (rng.NextFloat() >= GetAttributeValue(Attribute::SpawnReinforcements)) return true;

        const int x = static_cast<int>(std::floor(position.x));
        const int y = static_cast<int>(std::floor(position.y));
        const int z = static_cast<int>(std::floor(position.z));
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return true;

        // MC: `EntityType<? extends Zombie> type = this.getType()` — a husk
        // calls husks, a drowned calls drowned. (ZombifiedPiglin never gets
        // here: its reinforcements chance is pinned to 0, MC's own gate.)
        std::unique_ptr<Zombie> reinforcement;
        switch (GetType()) {
            case EntityTypeId::Husk:
                reinforcement = std::make_unique<Husk>(m_level); break;
            case EntityTypeId::Drowned:
                reinforcement = std::make_unique<Drowned>(m_level); break;
            case EntityTypeId::ZombieVillager:
                reinforcement = std::make_unique<ZombieVillager>(m_level); break;
            default:
                reinforcement = std::make_unique<Zombie>(m_level); break;
        }

        // MC: 50 attempts at nextInt(7,40) * nextInt(-1,1) offsets per axis.
        for (int i = 0; i < 50; ++i) {
            const int xt = x + rng.NextInt(7, 40) * rng.NextInt(-1, 1);
            const int yt = y + rng.NextInt(7, 40) * rng.NextInt(-1, 1);
            const int zt = z + rng.NextInt(7, 40) * rng.NextInt(-1, 1);

            // SpawnPlacements.isSpawnPositionOk + checkSpawnRules for THIS
            // zombie's type — which is ON_GROUND + checkMonsterSpawnRules.
            if (!IsSpawnPositionOk(GetType(), *blocks, xt, yt, zt)) continue;
            if (!CheckMonsterSpawnRules(*m_level, SpawnReason::Reinforcement,
                                        glm::ivec3(xt, yt, zt), rng)) {
                continue;
            }

            reinforcement->position = glm::dvec3(xt, yt, zt);
            // hasNearbyAlivePlayer(7) must be FALSE — reinforcements arrive
            // out of sight, not on top of the player.
            if (m_level->GetNearestPlayer(xt, yt, zt, 7.0)) continue;
            if (!reinforcement->CheckSpawnObstruction(*m_level)) continue;
            {
                // level.noCollision(reinforcement) — the block half.
                PhysicsContext phys = m_level->Physics();
                if (Game::CollidesAt(reinforcement->GetAABB(), phys)) continue;
            }

            reinforcement->SetTarget(target);
            reinforcement->FinalizeSpawn(SpawnReason::Reinforcement, nullptr);
            Zombie* placed = reinforcement.get();
            m_level->AddFreshEntity(std::move(reinforcement));

            // Both zombies' future reinforcement odds drop by 0.05 — the
            // caller via its accumulating "reinforcement_caller_charge"
            // modifier, the callee via the DISTINCT
            // "reinforcement_callee_charge" id (Zombie.java:294,502). The ids
            // must differ: a callee later promoted to caller stacks both,
            // and one id clobbered the other.
            double existing = 0.0;
            if (m_attributes.HasModifier(Attribute::SpawnReinforcements,
                                         ModifierId::ZombieSpawnReinf)) {
                // MC reads the modifier's amount back; the map has no getter
                // for that, so the accumulated charge is mirrored here.
                existing = m_reinforcementCallerCharge;
            }
            m_reinforcementCallerCharge = existing - 0.05;
            m_attributes.RemoveModifier(Attribute::SpawnReinforcements,
                                        ModifierId::ZombieSpawnReinf);
            m_attributes.AddModifier(Attribute::SpawnReinforcements,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::ZombieSpawnReinf),
                                   m_reinforcementCallerCharge, AttributeOperation::AddValue });
            placed->m_attributes.AddModifier(Attribute::SpawnReinforcements,
                AttributeModifier{ static_cast<uint32_t>(ModifierId::ZombieReinfCalleeCharge),
                                   -0.05, AttributeOperation::AddValue });
            break;
        }
        return true;
    }

    bool Zombie::DoHurtTarget(Entity& target) {
        // MC Zombie.doHurtTarget: super first, then the fire hand-off — an
        // on-fire, EMPTY-HANDED zombie ignites its victim for
        // 2 * (int)effectiveDifficulty seconds with chance
        // effectiveDifficulty * 0.3. No mob equipment system exists, so the
        // mainhand is always empty (also MC's common case). Regional
        // difficulty is not modelled; these are the fresh-world base values —
        // EASY 0.75, NORMAL 1.5, HARD 2.25 — the same convention the husk's
        // HUNGER hit uses.
        const bool result = Monster::DoHurtTarget(target);
        if (result && m_level && IsOnFire()) {
            float effectiveDifficulty = 0.0f;
            switch (m_level->GetDifficulty()) {
                case Difficulty::Easy:   effectiveDifficulty = 0.75f; break;
                case Difficulty::Normal: effectiveDifficulty = 1.5f;  break;
                case Difficulty::Hard:   effectiveDifficulty = 2.25f; break;
                default: break;
            }
            if (m_level->Random().NextFloat() < effectiveDifficulty * 0.3f) {
                target.IgniteForSeconds(2 * static_cast<int>(effectiveDifficulty));
            }
        }
        return result;
    }

    // ── Skeleton ───────────────────────────────────────────────────────────

    void Skeleton::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Skeleton::Skeleton(EntityLevel* level) : Skeleton(EntityTypeId::Skeleton, level) {
        RegisterGoals();
    }

    Skeleton::Skeleton(EntityTypeId type, EntityLevel* level) : Monster(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // RegisterGoals is the concrete constructor's job (Zombie pattern).

        // avoidSun is NOT pinned on here. MC toggles it from RestrictSunGoal
        // with the daylight; setting it once at construction made a skeleton
        // refuse sunlit paths at midnight too.
    }

    // ── WitherSkeleton ─────────────────────────────────────────────────────

    bool WitherSkeleton::DoHurtTarget(Entity& target) {
        // MC WitherSkeleton.doHurtTarget: super first, then WITHER for 200
        // ticks (amplifier 0) on any living target — no difficulty gate.
        if (!GenericMonster::DoHurtTarget(target)) return false;
        if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
            living->AddEffect(MobEffectInstance(MobEffectId::Wither, 200), this);
        }
        return true;
    }

    // ── Stray ──────────────────────────────────────────────────────────────

    Stray::Stray(EntityLevel* level) : Skeleton(EntityTypeId::Stray, level) {
        // MC Stray adds nothing to AbstractSkeleton's goals or attributes —
        // only the sounds, the spawn rule (SpawnPlacements' business) and the
        // tipped arrow below.
        RegisterGoals();
    }

    void Stray::CustomizeArrow(Arrow& arrow) {
        // MC Stray.getArrow: a SLOWNESS 600-tick (30 s) tip on every arrow.
        // Custom arrow effects apply at FULL duration — the /8 division is
        // for PotionContents on a crafted tipped arrow only.
        arrow.AddEffect(MobEffectInstance(MobEffectId::Slowness, 600));
    }

    // ── Bogged ─────────────────────────────────────────────────────────────

    void Bogged::CreateAttributes(AttributeMap& out) {
        Skeleton::CreateAttributes(out);
        out.Register(Attribute::MaxHealth, 16.0);
    }

    Bogged::Bogged(EntityLevel* level) : Skeleton(EntityTypeId::Bogged, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Bogged::CustomizeArrow(Arrow& arrow) {
        // MC Bogged.getArrow.
        arrow.AddEffect(MobEffectInstance(MobEffectId::Poison, 100));
    }

    void Skeleton::PerformRangedAttack(LivingEntity& target, float power) {
        if (!m_level) return;

        auto arrow = std::make_unique<Arrow>(m_level);
        arrow->SetOwner(this);
        arrow->position = glm::dvec3(position.x, GetEyeY() - 0.1, position.z);
        arrow->SetBaseDamageFromMob(power);
        // MC routes arrow creation through getArrow so subclasses can tip it.
        CustomizeArrow(*arrow);

        // MC: aim at a third of the way up the target's box, with a 0.2-per-
        // horizontal-block loft that compensates for gravity.
        const double xd = target.position.x - position.x;
        const double yd = (target.position.y + target.GetBbHeight() / 3.0) -
                          arrow->position.y;
        const double zd = target.position.z - position.z;
        const double horiz = std::sqrt(xd * xd + zd * zd);

        const int difficultyId = static_cast<int>(m_level->GetDifficulty());
        arrow->Shoot(xd, yd + horiz * 0.2, zd, 1.6f,
                     static_cast<float>(14 - difficultyId * 4));

        m_level->AddFreshEntity(std::move(arrow));
    }

    void Skeleton::RegisterGoals() {
        // MC AbstractSkeleton.registerGoals + reassessWeaponGoal's bow branch.
        //
        // The sun goals used to be described as "covered by the navigation's
        // avoidSun flag", but that flag was only ever set once at construction:
        // it made the skeleton route around sunlight day AND night, and it
        // never made a burning one actively run for shade. These are MC's.
        m_goalSelector.AddGoal(2, std::make_unique<RestrictSunGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<FleeSunGoal>(this, 1.0));
        // MC: 3 AvoidEntityGoal(Wolf.class, 6.0F, 1.0, 1.2).
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kWolfAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        // MC reassessWeaponGoal: getHardAttackInterval() on HARD (20; the
        // Bogged's 50), getAttackInterval() otherwise (40; Bogged 70).
        const int interval = (m_level && m_level->GetDifficulty() == Difficulty::Hard)
            ? GetHardAttackInterval() : GetAttackInterval();
        m_goalSelector.AddGoal(4, std::make_unique<RangedBowAttackGoal>(
                                      this, this, 1.0, interval, 15.0f));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackablePlayerGoal>(this, true));
        // MC: 3 NearestAttackableTargetGoal(IronGolem.class, true). (Turtle at
        // 3 waits on the baby-on-land selector.)
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kIronGolemTargets, 1, true));
    }

    // The Creeper/Wither explosion helpers that used to live here — a
    // getSeenPercent port and an entity-damage sweep — moved wholesale to
    // common/world/level/Explosion.cpp.
    //
    // Their own comment said "terrain destruction is deliberately absent ...
    // this is the single place to add it", and that is what happened: the
    // shared module now does the block half too, so a creeper leaves a crater.
    // Folding them out also killed the near-duplicate in
    // Projectile::ExplodeDamageOnly, which had no exposure raycast at all and
    // therefore let a ghast fireball deal full damage through a wall.

    // ── Creeper ────────────────────────────────────────────────────────────

    void Creeper::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Creeper::Creeper(EntityLevel* level) : Monster(EntityTypeId::Creeper, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Creeper::RegisterGoals() {
        // MC Creeper.registerGoals. Note SwellGoal at 2 outranks the melee
        // goal at 4: once the fuse is lit, nothing else moves the creeper. And
        // the ocelot/cat avoidance at 3 outranks BOTH melee and wandering —
        // a cat parks a creeper farm's output at the wall.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<SwellGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kOcelotAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kCatAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        // MC's creeper is the odd one out: NearestAttackableTarget at 1,
        // HurtBy at 2 — the inverse of most mobs.
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackablePlayerGoal>(this, true));
        m_targetSelector.AddGoal(2, std::make_unique<HurtByTargetGoal>(this));
    }

    void Creeper::SetTarget(LivingEntity* target) {
        // MC: creepers refuse goats as targets.
        if (target && target->GetType() == EntityTypeId::Goat) return;
        Monster::SetTarget(target);
    }

    void Creeper::SetSwellDir(int dir) { m_swellDir = dir; }

    float Creeper::GetSwelling(float partialTick) const {
        const float lerped = static_cast<float>(m_oldSwell) +
                             partialTick * static_cast<float>(m_swell - m_oldSwell);
        return lerped / static_cast<float>(kMaxSwell - 2);
    }

    void Creeper::Tick() {
        // MC runs the fuse BEFORE super.tick(), so a creeper that reaches full
        // swell explodes on the same tick rather than getting one more of AI.
        // The whole fuse block is gated on isAlive() (Creeper.java:121-143):
        // a creeper killed mid-hiss stops ticking its fuse through the
        // 20-tick death animation instead of exploding posthumously.
        if (IsAlive()) {
            m_oldSwell = m_swell;

            if (m_ignited) SetSwellDir(1);

            m_swell += m_swellDir;
            if (m_swell < 0) m_swell = 0;

            if (m_swell >= kMaxSwell) {
                m_swell = kMaxSwell;
                Explode();
            }
        }

        Monster::Tick();
    }

    void Creeper::Explode() {
        if (!IsEffectiveAi() || IsRemoved()) return;

        // MC Creeper.explodeCreeper: level.explode(this, getX, getY, getZ,
        // radius, ExplosionInteraction.MOB) — the creeper's FEET, not its eyes.
        //
        // MOB rather than TNT is what routes this through the mobGriefing
        // gamerule: with griefing off a creeper still hurts you and leaves the
        // terrain alone, which is decided inside Explode rather than here.
        ExplosionParams params;
        params.center       = position;
        params.radius       = static_cast<float>(kExplosionRadius);
        params.source       = this;
        params.attributedTo = this;
        params.interaction  = ExplosionInteraction::Mob;
        // Qualified: Creeper::Explode / the member we are inside shadows the
        // free function.
        Game::Explode(*m_level, params);

        // MC Creeper.explodeCreeper: spawnLingeringCloud() before the discard
        // — a creeper carrying ANY active effects (a witch's splash, a spider
        // pack potion gone astray) leaves them behind as a shrinking
        // AreaEffectCloud. A plain effect-free creeper leaves nothing, which
        // is the common case and MC's too.
        SpawnLingeringCloud();
        Discard();
    }

    void Creeper::SpawnLingeringCloud() {
        // MC Creeper.spawnLingeringCloud, verbatim numbers: radius 2.5
        // shrinking to zero over 300 ticks, -0.5 per application, 10-tick
        // wait, quarter-duration effects.
        if (ActiveEffects().empty() || !m_level) return;

        auto cloud = std::make_unique<AreaEffectCloud>(m_level);
        cloud->position = position;
        cloud->SetRadius(2.5f);
        cloud->SetRadiusOnUse(-0.5f);
        cloud->SetWaitTime(10);
        cloud->SetDuration(300);
        cloud->SetPotionDurationScale(0.25f);
        cloud->SetRadiusPerTick(-cloud->GetRadius() /
                                static_cast<float>(cloud->GetDuration()));
        for (const MobEffectInstance& fx : ActiveEffects()) {
            cloud->AddCloudEffect(MobEffectInstance(fx));
        }
        m_level->AddFreshEntity(std::move(cloud));
    }

    void Creeper::HandleEntityEvent(uint8_t id) {
        // The explosion visual used to be answered here off a stand-in event
        // byte. It now arrives as ExplodeS2C, which carries the exact centre
        // and the REAL destroyed-block count instead of a radius-cubed guess —
        // so this override no longer has an explosion case at all.
        Monster::HandleEntityEvent(id);
    }

    // ── Enderman ───────────────────────────────────────────────────────────

    void Enderman::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    40.0);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::AttackDamage,  7.0);
        out.Register(Attribute::FollowRange,  64.0);
        out.Register(Attribute::StepHeight,    1.0);
    }

    Enderman::Enderman(EntityLevel* level)
        : Monster(EntityTypeId::Enderman, level), NeutralMob(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetPathfindingMalus(PathType::Water, -1.0f);
        RegisterGoals();
    }

    void Enderman::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void Enderman::RegisterGoals() {
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<EndermanFreezeWhenLookedAt>(this));
        m_goalSelector.AddGoal(2, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        // MC: probability 0.0F — an enderman NEVER wanders into water on its
        // own; water is lethal to it.
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 1.0, 0.0f));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        m_goalSelector.AddGoal(10, std::make_unique<EndermanLeaveBlockGoal>(this));
        m_goalSelector.AddGoal(11, std::make_unique<EndermanTakeBlockGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<EndermanLookForPlayerGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<HurtByTargetGoal>(this));
        static constexpr EntityTypeId kEndermiteTargets[] = { EntityTypeId::Endermite };
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kEndermiteTargets, 1, true));
        m_targetSelector.AddGoal(4, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/false));
    }

    void Enderman::SetTarget(LivingEntity* target) {
        // MC EnderMan.setTarget (EnderMan.java:111-127): acquiring a target
        // raises the creepy flag (open jaw, shaking), stamps targetChangeTime
        // for the daylight-teleport grace period, and adds the transient
        // +0.15 "attacking" MOVEMENT_SPEED modifier — the sprint that makes
        // an angry enderman outrun a walking player. Losing the target zeroes
        // targetChangeTime and removes the modifier. The wire's aggressive
        // bit is what the renderer reads for the face.
        Monster::SetTarget(target);
        SetAggressive(target != nullptr);
        if (target == nullptr) {
            m_targetChangeTime = 0;
            m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                        ModifierId::EndermanAttackingSpeed);
        } else {
            m_targetChangeTime = tickCount;
            if (!m_attributes.HasModifier(Attribute::MovementSpeed,
                                          ModifierId::EndermanAttackingSpeed)) {
                m_attributes.AddModifier(Attribute::MovementSpeed,
                    AttributeModifier{
                        static_cast<uint32_t>(ModifierId::EndermanAttackingSpeed),
                        0.15, AttributeOperation::AddValue });
            }
        }
    }

    bool Enderman::IsBeingStaredBy(LivingEntity& player) {
        // MC isLookingAtMe(player, 0.025, scaleByDistance=true) at the
        // enderman's eye height. The carved-pumpkin disguise exemption needs
        // player equipment and is absent.
        const glm::vec3 viewF = Mth::ViewVector(player.xRot, player.yRot);
        glm::dvec3 view(viewF.x, viewF.y, viewF.z);
        view = glm::normalize(view);

        glm::dvec3 toMe(position.x - player.position.x,
                        GetEyeY() - player.GetEyeY(),
                        position.z - player.position.z);
        const double dist = glm::length(toMe);
        if (dist < 1.0e-8) return false;
        toMe /= dist;

        const double dot = glm::dot(view, toMe);
        if (dot <= 1.0 - 0.025 / dist) return false;
        return GetSensing().HasLineOfSight(player);
    }

    bool Enderman::Teleport() {
        if (!m_level || m_level->IsClientSide() || !IsAlive()) return false;
        JavaRandom& rng = m_level->Random();
        const double xx = position.x + (rng.NextDouble() - 0.5) * 64.0;
        const double yy = position.y + (rng.NextInt(64) - 32);
        const double zz = position.z + (rng.NextDouble() - 0.5) * 64.0;
        return TeleportTo(xx, yy, zz);
    }

    bool Enderman::TeleportTowards(const Entity& target) {
        glm::dvec3 dir(position.x - target.position.x,
                       position.y + GetBbHeight() * 0.5 - target.GetEyeY(),
                       position.z - target.position.z);
        const double len = glm::length(dir);
        if (len > 1.0e-8) dir /= len;

        JavaRandom& rng = m_level->Random();
        const double xx = position.x + (rng.NextDouble() - 0.5) * 8.0 - dir.x * 16.0;
        const double yy = position.y + (rng.NextInt(16) - 8) - dir.y * 16.0;
        const double zz = position.z + (rng.NextDouble() - 0.5) * 8.0 - dir.z * 16.0;
        return TeleportTo(xx, yy, zz);
    }

    bool Enderman::TeleportTo(double x, double y, double z) {
        const IBlockAccess* blocks = m_level->Blocks();
        if (!blocks) return false;

        // MC: descend to the first motion-blocking block, refuse wet landings.
        glm::ivec3 pos(static_cast<int>(std::floor(x)),
                       static_cast<int>(std::floor(y)),
                       static_cast<int>(std::floor(z)));
        while (pos.y > -64 &&
               !BlockRegistry::HasCollision(blocks->GetBlock(pos.x, pos.y, pos.z))) {
            --pos.y;
        }
        if (pos.y <= -64) return false;
        if (!BlockRegistry::HasCollision(blocks->GetBlock(pos.x, pos.y, pos.z))) return false;
        if (blocks->ContainsWater(pos.x, pos.y, pos.z) ||
            blocks->ContainsWater(pos.x, pos.y + 1, pos.z)) {
            return false;
        }

        // MC randomTeleport's placement test: the body must fit standing on
        // the found surface.
        const glm::dvec3 destination(x, pos.y + 1.0, z);
        const glm::vec3 half = HalfExtents();
        AABB box(glm::vec3(destination.x, destination.y + half.y, destination.z),
                 half * 2.0f);
        PhysicsContext phys = m_level->Physics();
        if (CollidesAt(box, phys)) return false;

        position = destination;
        velocity = glm::dvec3(0.0);
        ResetFallDistance();
        needsSync = true;
        return true;
    }

    bool Enderman::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC: projectiles never land — the enderman blinks away, up to 64
        // attempts. (Thrown water potions, the one projectile exception, need
        // the potion system.)
        if (source == MobDamageSource::Projectile) {
            for (int i = 0; i < 64; ++i) {
                if (Teleport()) return true;
            }
            return false;
        }

        const bool result = Monster::Hurt(source, amount, attacker);
        // Environmental damage (no living attacker) teleports 9 times in 10.
        if (m_level && !m_level->IsClientSide() &&
            !dynamic_cast<LivingEntity*>(attacker) &&
            m_level->Random().NextInt(10) != 0) {
            Teleport();
        }
        return result;
    }

    void Enderman::AiStep() {
        // MC EnderMan.aiStep (EnderMan.java:201): jumping is force-cleared
        // EVERY tick — an enderman never jumps, it teleports; without this a
        // jump latched by the shared jump control would carry over.
        jumping = false;
        // MC: updatePersistentAnger BEFORE super (the client half is the
        // portal particles — none yet).
        if (m_level && !m_level->IsClientSide()) {
            UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
        }
        // MC isSensitiveToWater: one drowning point per tick in contact.
        if (m_level && !m_level->IsClientSide() && IsAlive() && IsInWater()) {
            Hurt(MobDamageSource::Drown, 1.0f, nullptr);
        }
        Monster::AiStep();
    }

    void Enderman::CustomServerAiStep() {
        // MC: daylight teleport — the enderman's answer to sunrise, gated on
        // 600 ticks since it last acquired a target so a fight is not ended
        // by dawn mid-swing.
        if (m_level && m_level->IsDay() && tickCount >= m_targetChangeTime + kMinDeaggressionTime) {
            const glm::ivec3 p = BlockPosition();
            const float brightness = LightLevelDependentMagicValue(*m_level, p.x, p.y, p.z);
            if (brightness > 0.5f && m_level->CanSeeSky(p.x, p.y, p.z) &&
                m_level->Random().NextFloat() * 30.0f < (brightness - 0.4f) * 2.0f) {
                SetTarget(nullptr);
                Teleport();
            }
        }
        Monster::CustomServerAiStep();
    }

    void Enderman::DropCustomDeathLoot(EntityLevel& level) {
        // MC drops the carried block through its loot table with a fake tool;
        // for every ENDERMAN_HOLDABLE block the drop is the block itself.
        if (m_carriedBlock != BlockID::Air) {
            level.SpawnItemDrop(position, ItemRegistry::FromBlock(m_carriedBlock), 1);
            m_carriedBlock = BlockID::Air;
        }
    }

    // ── Spider ─────────────────────────────────────────────────────────────

    void Spider::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    16.0);
        out.Register(Attribute::MovementSpeed, 0.3);
    }

    Spider::Spider(EntityLevel* level) : Spider(EntityTypeId::Spider, level) {
        RegisterGoals();
    }

    Spider::Spider(EntityTypeId type, EntityLevel* level) : Monster(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC Spider.createNavigation: the wall climber — when A* has no route
        // to the target, steer straight at it and let climbing do the rest.
        SetNavigation(std::make_unique<WallClimberNavigation>(this, level));
        // RegisterGoals is the concrete constructor's job (Zombie pattern).
    }

    // ── CaveSpider ─────────────────────────────────────────────────────────

    void CaveSpider::CreateAttributes(AttributeMap& out) {
        // MC CaveSpider.createCaveSpider: Spider.createAttributes() +
        // MAX_HEALTH 12.
        Spider::CreateAttributes(out);
        out.Register(Attribute::MaxHealth, 12.0);
    }

    CaveSpider::CaveSpider(EntityLevel* level)
        : Spider(EntityTypeId::CaveSpider, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();   // Spider's goal set — MC adds none of its own
    }

    bool CaveSpider::DoHurtTarget(Entity& target) {
        // MC CaveSpider.doHurtTarget: super first, then the poison bite —
        // 7 s on NORMAL, 15 s on HARD, nothing on EASY (or peaceful).
        if (!Spider::DoHurtTarget(target)) return false;
        if (auto* living = dynamic_cast<LivingEntity*>(&target)) {
            int poisonSeconds = 0;
            if (m_level) {
                if (m_level->GetDifficulty() == Difficulty::Normal) poisonSeconds = 7;
                else if (m_level->GetDifficulty() == Difficulty::Hard) poisonSeconds = 15;
            }
            if (poisonSeconds > 0) {
                living->AddEffect(
                    MobEffectInstance(MobEffectId::Poison, poisonSeconds * 20, 0),
                    this);
            }
        }
        return true;
    }

    bool Spider::IsDarkEnoughToHunt(Mob& mob) {
        EntityLevel* level = mob.Level();
        if (!level) return false;
        // MC SpiderTargetGoal.canUse: getLightLevelDependentMagicValue()
        // >= 0.5F means passive. The magic value is the CURVED brightness at
        // the spider's EYE position — the curve crosses 0.5 at brightness 12,
        // so a spider hunts up to and including light 12, not up to 7.
        const glm::ivec3 p(static_cast<int>(std::floor(mob.position.x)),
                           static_cast<int>(std::floor(mob.position.y + mob.GetEyeHeight())),
                           static_cast<int>(std::floor(mob.position.z)));
        return LightLevelDependentMagicValue(*level, p.x, p.y, p.z) < 0.5f;
    }

    void Spider::RegisterGoals() {
        // MC Spider.registerGoals — including the armadillo avoidance at 2
        // (skipped only against a scared, rolled-up armadillo; the generic
        // armadillo here has no scared state yet).
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(this, kArmadilloAvoid, 1,
                                                                    6.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(3, std::make_unique<LeapAtTargetGoal>(this, 0.4f));
        m_goalSelector.AddGoal(4, std::make_unique<SpiderAttackGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.8));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(6, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<SpiderTargetGoal>(this, true));
        m_targetSelector.AddGoal(3, std::make_unique<SpiderTargetGoal>(
                                        this, kIronGolemTargets, 1, true));
    }

    // ── Guardian ───────────────────────────────────────────────────────────

    void Guardian::CreateAttributes(AttributeMap& out) {
        // MC Guardian.createAttributes: ATTACK_DAMAGE 6, MOVEMENT_SPEED 0.5,
        // MAX_HEALTH 30 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::AttackDamage,  6.0);
        out.Register(Attribute::MovementSpeed, 0.5);
        out.Register(Attribute::MaxHealth,    30.0);
    }

    Guardian::Guardian(EntityLevel* level) : Guardian(EntityTypeId::Guardian, level) {
        RegisterGoals();
    }

    Guardian::Guardian(EntityTypeId type, EntityLevel* level) : Monster(type, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor wiring: xpReward 10 (ours comes from the entity
        // type table), WATER path malus 0, the custom move control, and the
        // tail clock seeded so a shoal's tails start out of phase.
        SetPathfindingMalus(PathType::Water, 0.0f);
        SetMoveControl(std::make_unique<GuardianMoveControl>(this));
        SetNavigation(std::make_unique<WaterBoundPathNavigation>(this, level));
        if (level) m_tailAnimation = level->Random().NextFloat();
        m_tailAnimationO = m_tailAnimation;
        // RegisterGoals is called from the most-derived constructor only.
    }

    void Guardian::RegisterGoals() {
        // MC Guardian.registerGoals, priority for priority. MC sets MOVE|LOOK
        // on BOTH the restriction goal and the stroll so each owns the head
        // while steering.
        auto restriction = std::make_unique<MoveTowardsRestrictionGoal>(this, 1.0);
        restriction->SetFlags(GoalFlag::Move | GoalFlag::Look);
        auto stroll = std::make_unique<RandomStrollGoal>(this, 1.0, 80);
        m_randomStrollGoal = stroll.get();
        stroll->SetFlags(GoalFlag::Move | GoalFlag::Look);

        m_goalSelector.AddGoal(4, std::make_unique<GuardianAttackGoal>(this));
        m_goalSelector.AddGoal(5, std::move(restriction));
        m_goalSelector.AddGoal(7, std::move(stroll));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        // MC also looks at OTHER GUARDIANS (LookAtPlayerGoal(Guardian.class,
        // 12.0F, 0.01F) at 8); our LookAtPlayerGoal targets players only.
        m_goalSelector.AddGoal(9, std::make_unique<RandomLookAroundGoal>(this));

        // MC: 1 NearestAttackableTargetGoal(LivingEntity, 10, true, false,
        // GuardianAttackSelector) — players, squid and axolotls beyond 3
        // blocks. The selector-carrying port lives in GuardianGoals.
        m_targetSelector.AddGoal(1, std::make_unique<GuardianAttackTargetGoal>(this));
    }

    void Guardian::TriggerRandomStroll() {
        if (m_randomStrollGoal) m_randomStrollGoal->Trigger();
    }

    float Guardian::GetWalkTargetValue(const glm::ivec3& pos) const {
        // MC: water scores 10 + the light cost; everything else falls through
        // to the monster's dark preference.
        if (m_level && m_level->Blocks() &&
            m_level->Blocks()->ContainsWater(pos.x, pos.y, pos.z)) {
            return 10.0f + PathfindingCostFromLightLevels(*m_level, pos.x, pos.y, pos.z);
        }
        return Monster::GetWalkTargetValue(pos);
    }

    void Guardian::AiStep() {
        // MC Guardian.aiStep — everything below runs BEFORE super.aiStep().
        if (IsAlive()) {
            if (m_level && m_level->IsClientSide()) {
                // The client-side animation state, exactly MC's math. Our
                // client mobs run AiStep too, so this runs where MC runs it.
                m_tailAnimationO = m_tailAnimation;
                if (!IsInWater()) {
                    m_tailAnimationSpeed = 2.0f;
                    // MC plays the flop sound here when bouncing off the
                    // ground (the clientSideTouchedGround latch); that and the
                    // latch wait on the sound system.
                } else if (IsMoving()) {
                    if (m_tailAnimationSpeed < 0.5f) {
                        m_tailAnimationSpeed = 4.0f;
                    } else {
                        m_tailAnimationSpeed += (0.5f - m_tailAnimationSpeed) * 0.1f;
                    }
                } else {
                    m_tailAnimationSpeed += (0.125f - m_tailAnimationSpeed) * 0.2f;
                }
                m_tailAnimation += m_tailAnimationSpeed;

                m_spikesAnimationO = m_spikesAnimation;
                if (!IsInWater()) {
                    m_spikesAnimation = m_level->Random().NextFloat();
                } else if (IsMoving()) {
                    m_spikesAnimation += (0.0f - m_spikesAnimation) * 0.25f;
                } else {
                    m_spikesAnimation += (1.0f - m_spikesAnimation) * 0.06f;
                }

                // MC's swim-bubble trail and the beam's bubble line need the
                // particle system.

                // MC: the client counts its own attack time up toward the
                // duration while a beam target is synced — the renderer's
                // beam progress. Kept even with the beam visual skipped so
                // GetAttackAnimationScale answers correctly when it lands.
                if (HasActiveAttackTarget() &&
                    m_clientSideAttackTime < GetAttackDuration()) {
                    ++m_clientSideAttackTime;
                }
                // MC's per-particle beam line needs the particle system.
            }

            if (IsInWater()) {
                // MC setAirSupply(300) — no air-supply system yet.
            } else if (onGround && m_level) {
                // The beached hop: a random 0.4-scaled sideways jerk, 0.5 up,
                // and a fresh random facing every bounce.
                JavaRandom& rng = m_level->Random();
                velocity += glm::dvec3((rng.NextFloat() * 2.0f - 1.0f) * 0.4f,
                                       0.5,
                                       (rng.NextFloat() * 2.0f - 1.0f) * 0.4f);
                yRot = rng.NextFloat() * 360.0f;
                onGround = false;
                needsSync = true;
            }

            // MC: while the beam is charging the whole body turns with the
            // head, so the guardian faces what it is lasing.
            if (HasActiveAttackTarget()) {
                yRot = yHeadRot;
            }
        }

        Monster::AiStep();
    }

    void Guardian::Travel(const glm::dvec3& input) {
        // MC Guardian.travelInWater: moveRelative(0.1), raw move, 0.9 drag,
        // and a slow sink while idle with no target. Out of water the base
        // path applies, exactly as MC's travel dispatch does.
        if (IsInWater()) {
            MoveRelative(0.1f, input);
            Move(velocity);
            velocity *= 0.9;
            if (!IsMoving() && !GetTarget()) {
                velocity += glm::dvec3(0.0, -0.005, 0.0);
            }
            return;
        }
        Monster::Travel(input);
    }

    bool Guardian::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC hurtServer: with the spikes out (not moving), a melee hit costs
        // the attacker 2 thorns damage. MC keys on the damage source's DIRECT
        // entity being living (so arrows never trigger it) minus the
        // AVOIDS_GUARDIAN_THORNS tag; the melee sources are that same set
        // here. No thorns damage type exists, so the retaliation rides
        // Generic.
        if (m_level && !m_level->IsClientSide() && !IsMoving() &&
            (source == MobDamageSource::MobAttack ||
             source == MobDamageSource::PlayerAttack)) {
            if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
                living->Hurt(MobDamageSource::Generic, 2.0f, this);
            }
        }
        if (m_randomStrollGoal) m_randomStrollGoal->Trigger();
        return Monster::Hurt(source, amount, attacker);
    }

    bool Guardian::CheckSpawnObstruction(EntityLevel& level) const {
        // MC Guardian.checkSpawnObstruction: level.isUnobstructed(this) ONLY —
        // the base's containsAnyLiquid test would reject every underwater
        // spawn. The entity-overlap half below is the base's second test,
        // verbatim.
        const AABB box = GetAABB();
        std::vector<Entity*> occupants;
        level.GetEntitiesInBox(box, this, occupants);
        for (const Entity* other : occupants) {
            if (!other->IsRemoved() && other->GetAABB().Intersects(box)) return false;
        }
        return true;
    }

    // ── ElderGuardian ──────────────────────────────────────────────────────

    void ElderGuardian::CreateAttributes(AttributeMap& out) {
        // MC ElderGuardian.createAttributes: the guardian's, overridden to
        // MOVEMENT_SPEED 0.3, ATTACK_DAMAGE 8, MAX_HEALTH 80.
        Guardian::CreateAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.3);
        out.Register(Attribute::AttackDamage,  8.0);
        out.Register(Attribute::MaxHealth,    80.0);
    }

    ElderGuardian::ElderGuardian(EntityLevel* level)
        : Guardian(EntityTypeId::ElderGuardian, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: persistent, and the wander slowed to 400 ticks.
        SetPersistenceRequired(true);
        RegisterGoals();
        if (m_randomStrollGoal) m_randomStrollGoal->SetInterval(400);
    }

    void ElderGuardian::CustomServerAiStep() {
        Guardian::CustomServerAiStep();
        if (!m_level) return;

        // MC ElderGuardian.customServerAiStep: every EFFECT_INTERVAL (1200)
        // ticks, staggered by entity id so a monument's three elders do not
        // pulse in sync, Mining Fatigue III (amplifier 2) for 6000 ticks on
        // every survival player within 50 blocks.
        if ((tickCount + GetId()) % 1200 == 0) {
            std::vector<LivingEntity*> players;
            m_level->GetPlayers(players);
            for (LivingEntity* player : players) {
                if (!player || !player->IsAlive()) continue;
                // MobEffectUtil.addEffectToPlayersAround filters on
                // gameMode.isSurvival() — creative and spectator are exempt.
                if (player->IsCreative() || player->IsSpectator()) continue;
                if (DistanceToSqr(*player) >= 50.0 * 50.0) continue;

                // The util also skips players already carrying an equal-or-
                // stronger instance that will NOT end within the display
                // limit (1200 - 1 ticks) — the refresh cadence players see.
                const MobEffectInstance* existing =
                    player->GetEffect(MobEffectId::MiningFatigue);
                if (existing && existing->amplifier >= 2 &&
                    !existing->EndsWithin(1200 - 1)) {
                    continue;
                }

                player->AddEffect(
                    MobEffectInstance(MobEffectId::MiningFatigue, 6000, 2), this);
                // MC follows with ClientboundGameEventPacket
                // GUARDIAN_ELDER_EFFECT — the ghost-jumpscare overlay + sound.
                // No such packet exists; client sync is the documented
                // follow-up.
            }
        }

        // MC: an elder without a home anchors a 16-block one where it stands,
        // which is what MoveTowardsRestrictionGoal(5) then patrols back to.
        if (!HasHome()) {
            SetHomeTo(BlockPosition(), 16);
        }
    }

    std::shared_ptr<SpawnGroupData>
    Spider::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Monster::FinalizeSpawn(reason, std::move(groupData));
        if (!m_level) return groupData;
        JavaRandom& rng = m_level->Random();

        // MC: 1-in-100 skeleton jockey — spawn the skeleton and mount it
        // (Spider.finalizeSpawn: snapTo, finalizeSpawn(reason, null),
        // startRiding(this, force)).
        if (rng.NextInt(100) == 0) {
            auto skeleton = std::make_unique<Skeleton>(m_level);
            skeleton->position = position;
            skeleton->yRot = yRot;
            skeleton->yHeadRot = yRot;
            skeleton->yBodyRot = yRot;
            skeleton->FinalizeSpawn(reason, nullptr);
            Skeleton* placed = skeleton.get();
            m_level->AddFreshEntity(std::move(skeleton));
            placed->StartRiding(*this, /*force=*/true);
        }

        // MC Spider.SpiderEffectsGroupData: on HARD, 10%-of-difficulty chance
        // the whole pack shares one random effect — permanent (duration -1).
        struct SpiderEffectsGroupData : SpawnGroupData {
            bool        hasEffect = false;
            MobEffectId effect = MobEffectId::Speed;

            // MC setRandomEffect — nextInt(5), thresholds verbatim (SPEED gets
            // the double weight).
            void SetRandomEffect(JavaRandom& r) {
                const int selection = r.NextInt(5);
                if (selection <= 1)      effect = MobEffectId::Speed;
                else if (selection <= 2) effect = MobEffectId::Strength;
                else if (selection <= 3) effect = MobEffectId::Regeneration;
                else                     effect = MobEffectId::Invisibility;
                hasEffect = true;
            }
        };

        if (!groupData) {
            auto data = std::make_shared<SpiderEffectsGroupData>();
            if (m_level->GetDifficulty() == Difficulty::Hard &&
                rng.NextFloat() < 0.1f * GetSpecialMultiplier(m_level->GetDifficulty())) {
                data->SetRandomEffect(rng);
            }
            groupData = std::move(data);
        }

        // Every pack member — including the roller — reads the token.
        if (auto* effects = dynamic_cast<SpiderEffectsGroupData*>(groupData.get());
            effects && effects->hasEffect) {
            AddEffect(MobEffectInstance(effects->effect,
                                        MobEffectInstance::kInfiniteDuration));
        }
        return groupData;
    }

    void Spider::Tick() {
        Monster::Tick();
        // MC sets the climbing flag from horizontalCollision on the server and
        // syncs it; the client reads it to decide whether to draw the spider
        // flat against a wall.
        if (IsEffectiveAi()) m_climbing = horizontalCollision;
    }

    void Spider::Travel(const glm::dvec3& input) {
        Monster::Travel(input);

        // MC handles this through onClimbable() inside
        // handleRelativeFrictionAndCalculateMovement, which clamps vertical
        // motion to 0.2 upward while climbing. Applying it after the base
        // travel gives the same result for a spider (it has no other source of
        // upward motion) without threading a climbable flag through
        // LivingEntity for the one mob that uses it.
        if (m_climbing && (horizontalCollision || jumping)) {
            velocity.y = 0.2;
        }
    }

    // ── IronGolem ──────────────────────────────────────────────────────────

    namespace {

        // MC IronGolem's mob target predicate is
        // `target instanceof Enemy && !(target instanceof Creeper)`. There is
        // no Enemy marker interface here; MobCategory::Monster in the entity
        // type table is the same set (every Enemy spawns on the MONSTER pass —
        // slimes, phantoms and hoglins included), minus the creeper MC
        // excludes by name. Built once; NearestAttackableTargetGoal requires
        // the list to outlive the goal.
        const std::vector<EntityTypeId>& GolemEnemyTargets() {
            static const std::vector<EntityTypeId> kList = [] {
                std::vector<EntityTypeId> list;
                for (int i = 0; i < kEntityTypeCount; ++i) {
                    if (kEntityTypeTable[i].category != MobCategory::Monster) continue;
                    const auto type = static_cast<EntityTypeId>(i);
                    if (type == EntityTypeId::Creeper) continue;
                    list.push_back(type);
                }
                return list;
            }();
            return kList;
        }

    } // namespace

    void IronGolem::CreateAttributes(AttributeMap& out) {
        // MC IronGolem.createAttributes: MAX_HEALTH 100, MOVEMENT_SPEED 0.25,
        // KNOCKBACK_RESISTANCE 1.0, ATTACK_DAMAGE 15, STEP_HEIGHT 1.0 on the
        // mob base (AbstractGolem adds nothing).
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,          100.0);
        out.Register(Attribute::MovementSpeed,        0.25);
        out.Register(Attribute::KnockbackResistance,  1.0);
        out.Register(Attribute::AttackDamage,        15.0);
        out.Register(Attribute::StepHeight,           1.0);
    }

    IronGolem::IronGolem(EntityLevel* level)
        : PathfinderMob(EntityTypeId::IronGolem, level), NeutralMob(this) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void IronGolem::StartPersistentAngerTimer() {
        // MC PERSISTENT_ANGER_TIME = TimeUtil.rangeOfSeconds(20, 39).
        if (!m_level) return;
        SetTimeToRemainAngry(400 + m_level->Random().NextInt(381));
    }

    void IronGolem::RegisterGoals() {
        // MC IronGolem.registerGoals:
        //   2 MoveTowardsTargetGoal(0.9, 32.0F) — SKIPPED: the goal class is
        //     not implemented (only the iron golem uses it in MC). Its job —
        //     drift toward a target beyond melee range — is mostly covered by
        //     MeleeAttackGoal's pursuit.
        //   2 MoveBackToVillageGoal(0.6, false),
        //   4 GolemRandomStrollInVillageGoal(0.6),
        //   5 OfferFlowerGoal — SKIPPED: all three need the village system
        //     (POI sections, village boundaries, a villager to offer the poppy
        //     to). The offer-flower ANIMATION events (11/34) are handled below
        //     so a server that ever sends them renders correctly.
        //     GolemRandomStrollInVillageGoal is the golem's only wander, so a
        //     WaterAvoidingRandomStrollGoal at MC's 0.6 speed stands in for it
        //     until villages exist — without it a promoted golem would stand
        //     frozen forever.
        m_goalSelector.AddGoal(1, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(4, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(7, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        // MC targets:
        //   1 DefendVillageTargetGoal — SKIPPED with the village system.
        m_targetSelector.AddGoal(2, std::make_unique<HurtByTargetGoal>(this));
        // MC 3: NearestAttackableTargetGoal(Player, 10, true, false,
        // this::isAngryAt) — a golem only hunts players it is ANGRY at, and
        // anger only ever comes from being hit (HurtByTargetGoal's grudge is
        // what arms it, via updatePersistentAnger).
        auto angryAt = std::make_unique<NearestAttackableTargetGoal>(
            this, /*mustSee=*/true, /*mustReach=*/false, /*randomInterval=*/10);
        angryAt->SetSelector([](Mob& mob, const LivingEntity& target) {
            return static_cast<IronGolem&>(mob).IsAngryAt(target);
        });
        m_targetSelector.AddGoal(3, std::move(angryAt));
        const std::vector<EntityTypeId>& enemies = GolemEnemyTargets();
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, enemies.data(),
                                        static_cast<int>(enemies.size()),
                                        /*mustSee=*/false, /*mustReach=*/false,
                                        /*randomInterval=*/5));
        m_targetSelector.AddGoal(4, std::make_unique<ResetUniversalAngerTargetGoal>(
                                        this, /*alertOthersOfSameType=*/false));
    }

    bool IronGolem::CanAttack(const LivingEntity& target) const {
        // MC IronGolem.canAttack: creepers are never valid — attacking one
        // would blow up the village it defends. (The playerCreated exemption
        // needs the pumpkin-construction system.)
        if (target.GetType() == EntityTypeId::Creeper) return false;
        return PathfinderMob::CanAttack(target);
    }

    void IronGolem::AiStep() {
        // MC IronGolem.aiStep: super FIRST, then both clocks count down — on
        // the client too, which is what animates a remote golem's swing —
        // then updatePersistentAnger on the server.
        PathfinderMob::AiStep();
        if (m_attackAnimationTick > 0) {
            --m_attackAnimationTick;
        }
        if (m_offerFlowerTick > 0) {
            --m_offerFlowerTick;
        }
        if (m_level && !m_level->IsClientSide()) {
            UpdatePersistentAnger(/*stayAngryIfTargetPresent=*/true);
        }
    }

    bool IronGolem::DoHurtTarget(Entity& target) {
        // MC IronGolem.doHurtTarget, verbatim: arm the swing, tell every
        // watcher, then deal attackDamage/2 + nextInt(attackDamage) and launch
        // the victim 0.4 upward scaled by its knockback resistance.
        m_attackAnimationTick = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);

        const float attackDamage =
            static_cast<float>(GetAttributeValue(Attribute::AttackDamage));
        float damage = attackDamage;
        if (static_cast<int>(attackDamage) > 0 && m_level) {
            damage = attackDamage / 2.0f +
                     static_cast<float>(m_level->Random().NextInt(
                         static_cast<int>(attackDamage)));
        }

        LivingEntity* living = dynamic_cast<LivingEntity*>(&target);
        if (!living) return false;

        const bool hurt = living->Hurt(MobDamageSource::MobAttack, damage, this);
        if (hurt) {
            const double knockbackResistance =
                living->GetAttributeValue(Attribute::KnockbackResistance);
            const double scale = std::max(0.0, 1.0 - knockbackResistance);
            living->velocity.y += 0.4 * scale;
            living->hurtMarked = true;
            // MC: EnchantmentHelper.doPostAttackEffects — no enchantments.
            SetLastHurtMob(&target);
        }
        // MC plays IRON_GOLEM_ATTACK here — no sound system yet.
        return hurt;
    }

    void IronGolem::HandleEntityEvent(uint8_t id) {
        // MC IronGolem.handleEntityEvent, verbatim (the attack sound at 4
        // waits on the sound system; the offer-flower BEHAVIOUR needs
        // villagers, but the animation clock is the client's half and works).
        if (id == 4) {
            m_attackAnimationTick = 10;
        } else if (id == 11) {
            m_offerFlowerTick = 400;
        } else if (id == 34) {
            m_offerFlowerTick = 0;
        } else {
            PathfinderMob::HandleEntityEvent(id);
        }
    }

    // ── Ravager ────────────────────────────────────────────────────────────

    namespace {
        // MC Ravager targets AbstractVillager — villagers and the wandering
        // trader — plus iron golems, alongside players.
        constexpr EntityTypeId kRavagerVillagerTargets[] = {
            EntityTypeId::Villager, EntityTypeId::WanderingTrader };
        constexpr EntityTypeId kRavagerGolemTargets[] = { EntityTypeId::IronGolem };
        // MC's roar skips AbstractIllager — a ravager does not blast its own
        // raid party.
        constexpr EntityTypeId kIllagerTypes[] = {
            EntityTypeId::Evoker, EntityTypeId::Illusioner,
            EntityTypeId::Pillager, EntityTypeId::Vindicator };

        bool IsIllager(EntityTypeId type) {
            for (EntityTypeId t : kIllagerTypes) {
                if (t == type) return true;
            }
            return false;
        }
    } // namespace

    void Ravager::CreateAttributes(AttributeMap& out) {
        // MC Ravager.createAttributes: MAX_HEALTH 100, MOVEMENT_SPEED 0.3,
        // KNOCKBACK_RESISTANCE 0.75, ATTACK_DAMAGE 12, ATTACK_KNOCKBACK 1.5,
        // FOLLOW_RANGE 32, STEP_HEIGHT 1.0 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,          100.0);
        out.Register(Attribute::MovementSpeed,        0.3);
        out.Register(Attribute::KnockbackResistance,  0.75);
        out.Register(Attribute::AttackDamage,        12.0);
        out.Register(Attribute::AttackKnockback,      1.5);
        out.Register(Attribute::FollowRange,         32.0);
        out.Register(Attribute::StepHeight,           1.0);
    }

    Ravager::Ravager(EntityLevel* level) : Monster(EntityTypeId::Ravager, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: xpReward 20 (ours comes from the entity type
        // table) and a zero LEAVES malus — a ravager walks straight through
        // foliage.
        SetPathfindingMalus(PathType::Leaves, 0.0f);
        RegisterGoals();
    }

    void Ravager::RegisterGoals() {
        // MC Ravager.registerGoals. super.registerGoals() is
        // Raider.registerGoals (ObtainRaidLeaderBannerGoal, PathfindToRaidGoal,
        // RaiderMoveThroughVillageGoal, LongDistancePatrolGoal) — SKIPPED: all
        // four need the raid system.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<MeleeAttackGoal>(this, 1.0, true));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.4));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 8.0F) — our LookAtPlayerGoal
        // targets players only; the any-mob glance is not modelled.

        // MC 2 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers() —
        // the Raider exemption and the cross-type alert need the raid roster;
        // SetAlertOthers here wakes other ravagers, the same-species subset.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(2, std::move(hurtBy));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(this, true));
        // MC gates the villager goal on !target.isBaby(); TargetingConditions
        // has no baby filter yet, so baby villagers are targeted too (no baby
        // villagers exist here either).
        m_targetSelector.AddGoal(4, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kRavagerVillagerTargets, 2, true));
        m_targetSelector.AddGoal(4, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kRavagerGolemTargets, 1, true));
    }

    void Ravager::AiStep() {
        // MC Ravager.aiStep — super FIRST, then everything below.
        Monster::AiStep();
        if (!IsAlive()) return;

        // The hunt-speed lerp: rooted while a clock runs, otherwise the BASE
        // movement speed eases toward 0.35 with a target and 0.3 without
        // (BASE_MOVEMENT_SPEED / ATTACK_MOVEMENT_SPEED).
        if (IsImmobile()) {
            m_attributes.SetBaseValue(Attribute::MovementSpeed, 0.0);
        } else {
            const double maxSpeed = GetTarget() != nullptr ? 0.35 : 0.3;
            const double base = m_attributes.GetBaseValue(Attribute::MovementSpeed);
            m_attributes.SetBaseValue(Attribute::MovementSpeed,
                                      Mth::Lerp(0.1, base, maxSpeed));
        }

        // MC: with MOB_GRIEFING, a colliding ravager first smashes every
        // leaf block its box overlaps, and JUMPS when nothing broke. Block
        // destruction by mobs is not modelled, so only the jump half runs —
        // which is also MC's behaviour against any leaf-free wall. Server
        // only, matching MC's `level() instanceof ServerLevel` guard.
        if (m_level && !m_level->IsClientSide() && horizontalCollision && onGround) {
            JumpFromGround();
        }

        // The three clocks run on BOTH sides, exactly as MC's shared aiStep
        // does — the client's roarAnimation is derived from its own stun
        // countdown reaching zero, not from a packet. Roar() itself is
        // server-gated inside.
        if (m_roarTick > 0) {
            --m_roarTick;
            if (m_roarTick == 10) {
                Roar();
            }
        }

        if (m_attackTick > 0) {
            --m_attackTick;
        }

        if (m_stunnedTick > 0) {
            --m_stunnedTick;
            // MC stunEffect: the grey entity-effect particle — no particle
            // system yet.
            if (m_stunnedTick == 0) {
                // MC plays RAVAGER_ROAR here — no sound system yet.
                m_roarTick = 20;
            }
        }

        // MC hasLineOfSight is overridden to false while stunned or roaring
        // (so a mid-roar ravager cannot swing); Sensing has no per-mob hook,
        // but isImmobile() already roots it and MeleeAttackGoal cannot fire
        // while the attack clock it just armed is nonzero, so the observable
        // difference is nil until shields (the only stun source) exist.
        // MC blockedByItem (the shield stun + strong knockback) — SKIPPED:
        // no shield system.
    }

    void Ravager::Roar() {
        // MC Ravager.roar, server half: 6.0 mob-attack damage to every living
        // thing within 4 blocks (except other ravagers and illagers), a strong
        // knockback for the non-players, then event 69 for the client-side
        // knockback + particles. (The armor-stand and MOB_GRIEFING split in
        // the predicates has nothing to select between here.)
        if (!IsAlive() || !m_level || m_level->IsClientSide()) return;

        AABB box = GetAABB();
        box.min -= glm::vec3(4.0f);
        box.max += glm::vec3(4.0f);
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);

        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (living->GetType() == EntityTypeId::Ravager) continue;

            if (!IsIllager(living->GetType())) {
                living->Hurt(MobDamageSource::MobAttack, 6.0f, this);
            }
            if (!living->IsPlayer()) {
                StrongKnockback(*living);
            }
        }
        m_level->BroadcastEntityEvent(*this, 69);
    }

    void Ravager::StrongKnockback(Entity& entity) {
        // MC Ravager.strongKnockback, verbatim: a 4/d horizontal shove away
        // from the ravager plus 0.2 up (push() adds to the delta movement).
        const double xd = entity.position.x - position.x;
        const double zd = entity.position.z - position.z;
        const double dd = std::max(xd * xd + zd * zd, 0.001);
        entity.velocity += glm::dvec3(xd / dd * 4.0, 0.2, zd / dd * 4.0);
        entity.hurtMarked = true;
    }

    void Ravager::ApplyRoarKnockbackClient() {
        // MC applyRoarKnockbackClient: on the client only the LOCALLY
        // authoritative living entities (the local player) take the shove —
        // everything else is server-driven and got it in Roar().
        if (!m_level) return;
        AABB box = GetAABB();
        box.min -= glm::vec3(4.0f);
        box.max += glm::vec3(4.0f);
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);
        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (living->GetType() == EntityTypeId::Ravager) continue;
            if (!living->IsPlayer()) continue;
            StrongKnockback(*living);
        }
    }

    bool Ravager::DoHurtTarget(Entity& target) {
        // MC Ravager.doHurtTarget: arm the clock, broadcast, then the normal
        // hit (the base applies ATTACK_KNOCKBACK 1.5 as extra knockback).
        // MC plays RAVAGER_ATTACK here — no sound system yet.
        // MC getAttackBoundingBox deflates the reach box by 0.05 horizontally;
        // IsWithinMeleeAttackRange has no per-mob hook for a 5 cm trim.
        m_attackTick = 10;
        if (m_level) m_level->BroadcastEntityEvent(*this, 4);
        return Monster::DoHurtTarget(target);
    }

    void Ravager::HandleEntityEvent(uint8_t id) {
        // MC Ravager.handleEntityEvent, verbatim (the attack/roar sounds and
        // the 40 POOF roar particles wait on their systems).
        if (id == 4) {
            m_attackTick = 10;
        } else if (id == 39) {
            m_stunnedTick = 40;
        } else if (id == 69) {
            ApplyRoarKnockbackClient();
        } else {
            Monster::HandleEntityEvent(id);
        }
    }

    // ── Blaze ──────────────────────────────────────────────────────────────

    void Blaze::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::AttackDamage,  6.0);
        out.Register(Attribute::MovementSpeed, 0.23);
        out.Register(Attribute::FollowRange,  48.0);
    }

    Blaze::Blaze(EntityLevel* level) : Monster(EntityTypeId::Blaze, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor maluses: water forbidden, lava merely expensive,
        // fire costs nothing — a blaze lives in it.
        SetPathfindingMalus(PathType::Water, -1.0f);
        SetPathfindingMalus(PathType::Lava, 8.0f);
        SetPathfindingMalus(PathType::DangerFire, 0.0f);
        SetPathfindingMalus(PathType::DamageFire, 0.0f);
        RegisterGoals();
    }

    void Blaze::RegisterGoals() {
        // MC Blaze.registerGoals, priority for priority. The restriction goal
        // is dormant until something sets a home (in MC only a fortress
        // spawner-adjacent path would), exactly as in vanilla.
        m_goalSelector.AddGoal(4, std::make_unique<BlazeAttackGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<MoveTowardsRestrictionGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                      this, 1.0, 0.0f));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));

        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(
                                        this, /*mustSee=*/true));
    }

    void Blaze::AiStep() {
        // MC Blaze.aiStep head, BOTH sides: every falling tick is damped to
        // 60% — the hover. (The client-side burn sound and smoke particles
        // have no sound/particle system to land in.)
        if (!onGround && velocity.y < 0.0) {
            velocity.y *= 0.6;
        }
        Monster::AiStep();
    }

    void Blaze::CustomServerAiStep() {
        // MC Blaze.customServerAiStep: re-roll the allowed hover ceiling every
        // 100 ticks, and while the target's eyes are above it, thrust upward
        // at (0.3 - vy) * 0.3 per tick.
        if (--m_nextHeightOffsetChangeTick <= 0) {
            m_nextHeightOffsetChangeTick = 100;
            m_allowedHeightOffset = static_cast<float>(
                m_level->Random().Triangle(0.5, 6.891));
        }

        LivingEntity* target = GetTarget();
        if (target &&
            target->GetEyeY() > GetEyeY() + m_allowedHeightOffset &&
            CanAttack(*target)) {
            velocity.y += (0.3 - velocity.y) * 0.3;
            needsSync = true;
        }
        Monster::CustomServerAiStep();
    }

    // ── Ghast ──────────────────────────────────────────────────────────────

    void Ghast::CreateAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,    10.0);
        out.Register(Attribute::FollowRange, 100.0);
        out.Register(Attribute::FlyingSpeed,  0.06);
    }

    Ghast::Ghast(EntityLevel* level) : Mob(EntityTypeId::Ghast, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        SetMoveControl(std::make_unique<GhastMoveControl>(this));
        RegisterGoals();
    }

    void Ghast::RegisterGoals() {
        m_goalSelector.AddGoal(5, std::make_unique<RandomFloatAroundGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<GhastLookGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<GhastShootFireballGoal>(this));
        // MC: NearestAttackableTargetGoal(Player, 10, true, false, |dy| <= 4)
        // — the Y filter lives in CanAttack (see the header note).
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(
                                        this, /*mustSee=*/true, /*mustReach=*/false,
                                        /*randomInterval=*/10));
    }

    bool Ghast::CanAttack(const LivingEntity& target) const {
        return Mob::CanAttack(target) &&
               std::abs(target.position.y - position.y) <= 4.0;
    }

    void Ghast::Travel(const glm::dvec3& input) {
        // MC LivingEntity.travelFlying(input, 0.02F) — no gravity anywhere in
        // it, which is what keeps a ghast aloft. The onGround block-friction
        // variant of the air branch is dropped: a ghast is never on the ground.
        if (IsInWater()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.8;
        } else if (IsInLava()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.5;
        } else {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.91;
        }
    }

    void Ghast::FaceMovementDirection(Mob& ghast) {
        // MC Ghast.faceMovementDirection. The negated atan2 is MC's entity
        // yaw convention (0 = +Z, clockwise) — projectiles use the positive
        // form because their renderers compensate.
        if (!ghast.GetTarget()) {
            ghast.yRot = -static_cast<float>(
                std::atan2(ghast.velocity.x, ghast.velocity.z)) *
                Mth::kRadToDeg;
            ghast.yBodyRot = ghast.yRot;
        } else {
            LivingEntity* target = ghast.GetTarget();
            if (target->DistanceToSqr(ghast) < 4096.0) {
                const double xdd = target->position.x - ghast.position.x;
                const double zdd = target->position.z - ghast.position.z;
                ghast.yRot = -static_cast<float>(std::atan2(xdd, zdd)) *
                             Mth::kRadToDeg;
                ghast.yBodyRot = ghast.yRot;
            }
        }
    }

    // ── SnowGolem ──────────────────────────────────────────────────────────

    namespace {
        // MC SnowGolem's target selector: any Mob that is an Enemy — unlike
        // the iron golem's list, creepers included (only the golem's own
        // canAttack spares them, and a snow golem has no such override).
        const std::vector<EntityTypeId>& SnowGolemEnemyTargets() {
            static const std::vector<EntityTypeId> kList = [] {
                std::vector<EntityTypeId> list;
                for (int i = 0; i < kEntityTypeCount; ++i) {
                    if (kEntityTypeTable[i].category != MobCategory::Monster) continue;
                    list.push_back(static_cast<EntityTypeId>(i));
                }
                return list;
            }();
            return kList;
        }
    } // namespace

    void SnowGolem::CreateAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth,     4.0);
        out.Register(Attribute::MovementSpeed, 0.2);
    }

    SnowGolem::SnowGolem(EntityLevel* level)
        : PathfinderMob(EntityTypeId::SnowGolem, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void SnowGolem::RegisterGoals() {
        m_goalSelector.AddGoal(1, std::make_unique<RangedAttackGoal>(
                                      this, this, 1.25, 20, 10.0f));
        m_goalSelector.AddGoal(2, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                      this, 1.0, 1.0000001e-5f));
        m_goalSelector.AddGoal(3, std::make_unique<LookAtPlayerGoal>(this, 6.0f));
        m_goalSelector.AddGoal(4, std::make_unique<RandomLookAroundGoal>(this));

        const std::vector<EntityTypeId>& enemies = SnowGolemEnemyTargets();
        m_targetSelector.AddGoal(1, std::make_unique<NearestAttackableTargetGoal>(
                                        this, enemies.data(),
                                        static_cast<int>(enemies.size()),
                                        /*mustSee=*/true, /*mustReach=*/false,
                                        /*randomInterval=*/10));
    }

    void SnowGolem::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_level) return;

        auto snowball = std::make_unique<Snowball>(m_level);
        snowball->SetOwnerAndPosition(*this);

        // MC SnowGolem.performRangedAttack — note yd is the target's ABSOLUTE
        // eye height minus 1.1; the shoot call subtracts the snowball's own Y.
        const double xd = target.position.x - position.x;
        const double yd = target.GetEyeY() - 1.1;
        const double zd = target.position.z - position.z;
        const double yo = std::sqrt(xd * xd + zd * zd) * 0.2;

        snowball->Shoot(xd, yd + yo - snowball->position.y, zd, 1.6f, 12.0f);
        m_level->AddFreshEntity(std::move(snowball));
    }

    void SnowGolem::AiStep() {
        PathfinderMob::AiStep();
        if (!m_level || m_level->IsClientSide() || !IsAlive()) return;

        // MC SNOW_GOLEM_MELTS — biome base temperature above 1.0 melts the
        // golem at 1 fire damage per tick. (The water half of MC's
        // isSensitiveToWater damage runs in Mob::AiStep.)
        const glm::ivec3 p = BlockPosition();
        if (m_level->GetBiomeTemperature(p.x, p.y, p.z) > 1.0f) {
            Hurt(MobDamageSource::Fire, 1.0f, nullptr);
        }

        // MC leaves a snow-layer trail here (mobGriefing-gated); no snow
        // layer block exists in this engine yet.
    }

    // ── Witch ──────────────────────────────────────────────────────────────

    void Witch::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    26.0);
        out.Register(Attribute::MovementSpeed, 0.25);
    }

    Witch::Witch(EntityLevel* level) : Monster(EntityTypeId::Witch, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Witch::RegisterGoals() {
        // MC Witch.registerGoals (the Raider base's raid goals are the raid
        // system's and are skipped with it):
        //   target 2 NearestHealableRaiderTargetGoal — SKIPPED with raids.
        m_goalSelector.AddGoal(1, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<RangedAttackGoal>(
                                      this, this, 1.0, 60, 10.0f));
        m_goalSelector.AddGoal(2, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                      this, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(3, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableWitchTargetGoal>(
                                        this, /*mustSee=*/true, /*mustReach=*/false,
                                        /*randomInterval=*/10));
    }

    void Witch::AiStep() {
        // MC Witch.aiStep — the drink state machine runs BEFORE super, server
        // side, while alive. (The healRaidersGoal cooldown juggling at the top
        // waits on raids.)
        if (m_level && !m_level->IsClientSide() && IsAlive()) {
            JavaRandom& rng = m_level->Random();

            if (m_isDrinking) {
                if (m_usingTime-- <= 0) {
                    // Drink complete: MC reads the effects back off the
                    // main-hand potion stack; the pending list stands in for
                    // the stack (no item system).
                    m_isDrinking = false;
                    for (const MobEffectInstance& e : m_drinkPotion) {
                        AddEffect(e, this);
                    }
                    m_drinkPotion.clear();
                    m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                                ModifierId::WitchDrinkingSlowdown);
                }
            } else {
                // MC's pick chain — each else-if draws its own nextFloat, so
                // the RNG stream shape matches the original exactly.
                //
                // Potion payloads are Potions.java verbatim: water breathing
                // 3600, fire resistance 3600, healing (instant, 1), swiftness
                // 3600.
                std::vector<MobEffectInstance> potion;
                if (rng.NextFloat() < 0.15f && IsInWater() &&
                    !HasEffect(MobEffectId::WaterBreathing)) {
                    // MC tests isEyeInFluid(WATER) — the drowning position.
                    // No per-block fluid heights exist; IsInWater is the
                    // closest test. The air-supply system this potion guards
                    // against is itself a later wave, but the branch is live
                    // so the witch's behaviour (and RNG draws) already match.
                    potion.emplace_back(MobEffectId::WaterBreathing, 3600);
                } else if (rng.NextFloat() < 0.15f &&
                           (IsOnFire() ||
                            (HasLastDamageSource() &&
                             GetLastDamageSource() == MobDamageSource::Fire)) &&
                           !HasEffect(MobEffectId::FireResistance)) {
                    potion.emplace_back(MobEffectId::FireResistance, 3600);
                } else if (rng.NextFloat() < 0.05f && GetHealth() < GetMaxHealth()) {
                    potion.emplace_back(MobEffectId::InstantHealth, 1);
                } else if (rng.NextFloat() < 0.5f && GetTarget() &&
                           !HasEffect(MobEffectId::Speed) &&
                           GetTarget()->DistanceToSqr(*this) > 121.0) {
                    potion.emplace_back(MobEffectId::Speed, 3600);
                }

                if (!potion.empty()) {
                    m_drinkPotion = std::move(potion);
                    // MC: usingTime = getMainHandItem().getUseDuration() — 32
                    // ticks for a potion. The bottle-in-hand visual and the
                    // DATA_USING_ITEM sync wait on mob equipment/client sync.
                    m_usingTime = 32;
                    m_isDrinking = true;
                    // MC SPEED_MODIFIER_DRINKING: −0.25 ADD_VALUE while the
                    // bottle is up (remove-then-add, as MC does).
                    m_attributes.RemoveModifier(Attribute::MovementSpeed,
                                                ModifierId::WitchDrinkingSlowdown);
                    m_attributes.AddModifier(
                        Attribute::MovementSpeed,
                        AttributeModifier{
                            static_cast<uint32_t>(ModifierId::WitchDrinkingSlowdown),
                            -0.25, AttributeOperation::AddValue });
                }
            }

            // MC: the ambient witch-particle burst (entity event 15),
            // answered client-side by HandleEntityEvent below.
            if (rng.NextFloat() < 7.5e-4f) {
                m_level->BroadcastEntityEvent(*this, 15);
            }
        }

        Monster::AiStep();
    }

    void Witch::HandleEntityEvent(uint8_t id) {
        if (id == 15) {
            // MC Witch.handleEntityEvent(15): nextInt(35) + 10 WITCH
            // particles clustered at the hat tip (bb.maxY + 0.5), spread
            // gaussian * 0.13, zero velocity.
            if (!m_level) return;
            JavaRandom& rng = m_level->Random();
            const double maxY =
                position.y + static_cast<double>(GetBbHeight());
            const int count = rng.NextInt(35) + 10;
            for (int i = 0; i < count; ++i) {
                m_level->AddParticle(ParticleKind::WitchMagic,
                                     position.x + rng.NextGaussian() * 0.13,
                                     maxY + 0.5 + rng.NextGaussian() * 0.13,
                                     position.z + rng.NextGaussian() * 0.13,
                                     0.0, 0.0, 0.0);
            }
        } else {
            Monster::HandleEntityEvent(id);
        }
    }

    float Witch::GetDamageAfterMagicAbsorb(MobDamageSource source, float amount,
                                           Entity* attacker) const {
        // MC Witch.getDamageAfterMagicAbsorb: super, then self-damage zeroed
        // (its own splash potion), then WITCH_RESISTANT_TO — magic damage —
        // cut to 15%.
        amount = Monster::GetDamageAfterMagicAbsorb(source, amount, attacker);
        if (attacker == this) amount = 0.0f;
        if (source == MobDamageSource::Magic) amount *= 0.15f;
        return amount;
    }

    void Witch::PerformRangedAttack(LivingEntity& target, float power) {
        (void)power;
        if (!m_level) return;
        // MC gates on !isDrinkingPotion() — a witch mid-drink holds its fire.
        if (m_isDrinking) return;

        auto potion = std::make_unique<ThrownSplashPotion>(m_level);
        potion->SetOwnerAndPosition(*this);

        // MC leads the target by one tick of its velocity; yd is measured
        // from the WITCH's feet, exactly as in Witch.performRangedAttack.
        const double xd = target.position.x + target.velocity.x - position.x;
        const double yd = target.GetEyeY() - 1.1 - position.y;
        const double zd = target.position.z + target.velocity.z - position.z;
        const double dist = std::sqrt(xd * xd + zd * zd);

        // MC's potion pick, in order. HARMING is the default; the Raider
        // branch (healing/regeneration for hurt raiders, which also clears
        // the target) waits on raids. Payloads are Potions.java verbatim:
        // slowness 1800, poison 900, weakness 1800, harming instant.
        std::vector<MobEffectInstance> effects;
        if (dist >= 8.0 && !target.HasEffect(MobEffectId::Slowness)) {
            effects.emplace_back(MobEffectId::Slowness, 1800);
        } else if (target.GetHealth() >= 8.0f &&
                   !target.HasEffect(MobEffectId::Poison)) {
            effects.emplace_back(MobEffectId::Poison, 900);
        } else if (dist <= 3.0 && !target.HasEffect(MobEffectId::Weakness) &&
                   m_level->Random().NextFloat() < 0.25f) {
            effects.emplace_back(MobEffectId::Weakness, 1800);
        } else {
            effects.emplace_back(MobEffectId::InstantDamage, 1);
        }
        potion->SetEffects(std::move(effects));

        potion->Shoot(xd, yd + dist * 0.2, zd, 0.75f, 8.0f);
        m_level->AddFreshEntity(std::move(potion));
    }

    // ── Shulker ────────────────────────────────────────────────────────────

    namespace {
        // MC Direction ordinals: DOWN, UP, NORTH, SOUTH, WEST, EAST.
        constexpr glm::ivec3 kShulkerDirStep[6] = {
            { 0, -1,  0 }, { 0, 1, 0 }, { 0, 0, -1 },
            { 0,  0,  1 }, { -1, 0, 0 }, { 1, 0, 0 },
        };
        constexpr int kShulkerDirAxis[6] = { 1, 1, 2, 2, 0, 0 };
    } // namespace

    void Shulker::CreateAttributes(AttributeMap& out) {
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth, 30.0);
        // Registered so the covered-lid bonus has an instance to write.
        out.Register(Attribute::Armor, 0.0);
    }

    Shulker::Shulker(EntityLevel* level)
        : PathfinderMob(EntityTypeId::Shulker, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
        UpdateCoveredArmor();
    }

    void Shulker::RegisterGoals() {
        m_goalSelector.AddGoal(1, std::make_unique<LookAtPlayerGoal>(
                                      this, 8.0f, 0.02f, /*onlyHorizontal=*/true));
        m_goalSelector.AddGoal(4, std::make_unique<ShulkerAttackGoal>(this));
        m_goalSelector.AddGoal(7, std::make_unique<ShulkerPeekGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));

        // MC: HurtByTargetGoal(this, Shulker.class).setAlertOthers() — the
        // class argument EXCLUDES other shulkers from retaliation; alerting
        // still pulls them in.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<ShulkerNearestAttackGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<ShulkerDefenseAttackGoal>(this));
    }

    void Shulker::UpdateCoveredArmor() {
        // MC COVERED_ARMOR_MODIFIER: +20 armor while the lid is closed.
        m_attributes.SetBaseValue(Attribute::Armor, IsClosed() ? 20.0 : 0.0);
    }

    void Shulker::SetRawPeekAmount(int amount) {
        m_peekAmount = std::clamp(amount, 0, 100);
        UpdateCoveredArmor();
    }

    int Shulker::GetAttachAxis() const {
        return kShulkerDirAxis[m_attachFace];
    }

    bool Shulker::CanStayAt(const glm::ivec3& pos, int face) const {
        if (!m_level || !m_level->Blocks()) return false;
        const IBlockAccess& blocks = *m_level->Blocks();

        // MC isPositionBlocked: the shulker's own cell must be air. (The
        // moving-piston exemption goes with pistons.)
        if (blocks.GetBlock(pos.x, pos.y, pos.z) != BlockID::Air) return false;

        // MC: the block on the attach face must be sturdy. (The fully-open
        // no-collision box check is approximated away — the cell-is-air test
        // covers the common cases without a shaped-collision sweep.)
        const glm::ivec3 support = pos + kShulkerDirStep[face];
        return BlockRegistry::HasCollision(
            blocks.GetBlock(support.x, support.y, support.z));
    }

    int Shulker::FindAttachableSurface(const glm::ivec3& pos) const {
        for (int face = 0; face < 6; ++face) {
            if (CanStayAt(pos, face)) return face;
        }
        return -1;
    }

    void Shulker::FindNewAttachment() {
        const int face = FindAttachableSurface(BlockPosition());
        if (face != -1) {
            m_attachFace = face;
            needsSync = true;
        } else {
            TeleportSomewhere();
        }
    }

    bool Shulker::TeleportSomewhere() {
        // MC Shulker.teleportSomewhere: 5 attempts at ±8 blocks each axis.
        if (IsNoAi() || !IsAlive() || !m_level) return false;
        JavaRandom& rng = m_level->Random();
        const glm::ivec3 current = BlockPosition();

        for (int attempt = 0; attempt < 5; ++attempt) {
            const glm::ivec3 target = current + glm::ivec3(rng.NextInt(-8, 8),
                                                           rng.NextInt(-8, 8),
                                                           rng.NextInt(-8, 8));
            // MC also checks the world border and noCollision of the box —
            // the empty-cell test inside canStayAt covers the practical cases.
            if (target.y <= -64) continue;
            const int face = FindAttachableSurface(target);
            if (face == -1) continue;

            m_attachFace = face;
            position = glm::dvec3(target.x + 0.5, target.y, target.z + 0.5);
            SetRawPeekAmount(0);
            SetTarget(nullptr);
            needsSync = true;
            return true;
        }
        return false;
    }

    bool Shulker::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        // MC: a CLOSED shulker deflects arrows outright. APPROXIMATION,
        // noted in the header: the damage path does not say which projectile
        // hit, so every projectile bounces — in MC a shulker bullet's damage
        // would land (and trigger the split this port skips).
        if (IsClosed() && source == MobDamageSource::Projectile) return false;

        if (!PathfinderMob::Hurt(source, amount, attacker)) return false;

        if (GetHealth() < GetMaxHealth() * 0.5f && m_level &&
            m_level->Random().NextInt(4) == 0) {
            TeleportSomewhere();
        }
        return true;
    }

    void Shulker::Tick() {
        Mob::Tick();
        if (!m_level) return;

        if (!m_level->IsClientSide()) {
            // MC tick(): stay attached or find a new wall/teleport.
            if (!CanStayAt(BlockPosition(), m_attachFace)) FindNewAttachment();

            // MC Shulker.setPos floors every coordinate to the block centre —
            // a shulker is never partway between cells — and its
            // setDeltaMovement is hardwired to zero.
            position.x = std::floor(position.x) + 0.5;
            position.y = std::floor(position.y + 0.5);
            position.z = std::floor(position.z) + 0.5;
            velocity = glm::dvec3(0.0);
        }

        // MC updatePeekAmount — both sides: the lid chases the synced raw
        // peek at 0.05 per tick; the renderer reads the lerped pair.
        m_currentPeekAmountO = m_currentPeekAmount;
        const float target = static_cast<float>(m_peekAmount) * 0.01f;
        if (m_currentPeekAmount > target) {
            m_currentPeekAmount =
                std::clamp(m_currentPeekAmount - 0.05f, target, 1.0f);
        } else {
            m_currentPeekAmount =
                std::clamp(m_currentPeekAmount + 0.05f, 0.0f, target);
        }

        // MC ShulkerBodyRotationControl.clientTick is empty — the shell
        // never turns; only the head (yHeadRot) tracks the look target.
        yBodyRot = yBodyRotO = 0.0f;
    }

    // ── Phantom ────────────────────────────────────────────────────────────

    void Phantom::CreateAttributes(AttributeMap& out) {
        // MC DefaultAttributes registers plain Monster.createMonsterAttributes
        // for the phantom; ATTACK_DAMAGE is re-based to 6 + size by
        // updatePhantomSizeInfo (SetPhantomSize below).
        CreateMonsterAttributes(out);
    }

    Phantom::Phantom(EntityLevel* level) : Mob(EntityTypeId::Phantom, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor wiring: xpReward 5 (ours comes from the entity
        // type table), the three custom controls, CIRCLE phase.
        SetMoveControl(std::make_unique<PhantomMoveControl>(this));
        SetLookControl(std::make_unique<PhantomLookControl>(this));
        SetBodyRotationControl(std::make_unique<PhantomBodyRotationControl>(this));
        SetPhantomSize(0);
        RegisterGoals();
    }

    void Phantom::RegisterGoals() {
        // MC Phantom.registerGoals, priority for priority.
        m_goalSelector.AddGoal(1, std::make_unique<PhantomAttackStrategyGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<PhantomSweepAttackGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<PhantomCircleAroundAnchorGoal>(this));
        m_targetSelector.AddGoal(1, std::make_unique<PhantomAttackPlayerTargetGoal>(this));
    }

    void Phantom::SetPhantomSize(int size) {
        // MC setPhantomSize clamps 0..64; updatePhantomSizeInfo re-bases the
        // attack damage to 6 + size (the box scale lives in GetBbWidth/Height
        // and the renderer's model scale).
        m_size = std::clamp(size, 0, 64);
        m_attributes.SetBaseValue(Attribute::AttackDamage,
                                  6.0 + static_cast<double>(m_size));
    }

    void Phantom::Travel(const glm::dvec3& input) {
        // MC Phantom.travel: travelFlying(input, 0.2F) — no gravity, air drag
        // 0.91 (0.8 in water, 0.5 in lava). Same shape as the ghast's 0.02.
        if (IsInWater()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.8;
        } else if (IsInLava()) {
            MoveRelative(0.02f, input);
            Move(velocity);
            velocity *= 0.5;
        } else {
            MoveRelative(0.2f, input);
            Move(velocity);
            velocity *= 0.91;
        }
    }

    std::shared_ptr<SpawnGroupData>
    Phantom::FinalizeSpawn(SpawnReason reason,
                           std::shared_ptr<SpawnGroupData> groupData) {
        // MC Phantom.finalizeSpawn: anchor five blocks above the spawn point,
        // size 0, then super.
        SetAnchorPoint(BlockPosition() + glm::ivec3(0, 5, 0));
        SetPhantomSize(0);
        return Mob::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── SpellcasterIllager ─────────────────────────────────────────────────

    SpellcasterIllager::SpellcasterIllager(EntityTypeId type, EntityLevel* level)
        : Monster(type, level) {}

    bool SpellcasterIllager::IsCastingSpell() const {
        // MC isCastingSpell: the client answers from the synced spell id, the
        // server from its own tick counter.
        if (m_level && m_level->IsClientSide()) {
            return m_clientSpellId != 0;
        }
        return m_spellCastingTickCount > 0;
    }

    SpellcasterIllager::IllagerSpell SpellcasterIllager::GetCurrentSpell() const {
        if (m_level && m_level->IsClientSide()) {
            return static_cast<IllagerSpell>(m_clientSpellId);
        }
        return m_currentSpell;
    }

    void SpellcasterIllager::CustomServerAiStep() {
        // MC SpellcasterIllager.customServerAiStep: super, then count the
        // cast timer down.
        Monster::CustomServerAiStep();
        if (m_spellCastingTickCount > 0) {
            --m_spellCastingTickCount;
        }
    }

    // ── Evoker ─────────────────────────────────────────────────────────────

    namespace {
        constexpr EntityTypeId kCreakingAvoid[] = { EntityTypeId::Creaking };
    }

    void Evoker::CreateAttributes(AttributeMap& out) {
        // MC Evoker.createAttributes: MOVEMENT_SPEED 0.5, FOLLOW_RANGE 12,
        // MAX_HEALTH 24 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.5);
        out.Register(Attribute::FollowRange,  12.0);
        out.Register(Attribute::MaxHealth,    24.0);
    }

    Evoker::Evoker(EntityLevel* level)
        : SpellcasterIllager(EntityTypeId::Evoker, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC xpReward 10 — ours comes from the entity type table.
        RegisterGoals();
    }

    void Evoker::RegisterGoals() {
        // MC Evoker.registerGoals, priority for priority.
        // super.registerGoals() is Raider/AbstractIllager machinery (raid
        // goals) — SKIPPED with the raid system.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<EvokerCastingSpellGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<AvoidEntityGoal>(this, 8.0f, 0.6, 1.0));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kCreakingAvoid, 1,
                                                                    8.0f, 0.6, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<EvokerSummonSpellGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<EvokerAttackSpellGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<EvokerWololoSpellGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<RandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 8.0F) — our LookAtPlayerGoal
        // targets players only; the any-mob glance is not modelled.

        // MC 1 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers() —
        // the Raider exemption needs the raid roster; SetAlertOthers wakes
        // other evokers, the same-species subset.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        // MC: player target with a 300-tick unseen memory.
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoalWithMemory>(
                                        this, /*mustSee=*/true, 300));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoalWithMemory>(
                                        this, kVillagerTargets, 2,
                                        /*mustSee=*/false, 300));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kIronGolemTargets, 1,
                                        /*mustSee=*/false));
    }

    void Evoker::ClearReferenceTo(const Entity* entity) {
        SpellcasterIllager::ClearReferenceTo(entity);
        if (m_wololoTarget == entity) m_wololoTarget = nullptr;
    }

    // ── Vindicator ─────────────────────────────────────────────────────────

    void Vindicator::CreateAttributes(AttributeMap& out) {
        // MC Vindicator.createAttributes: MOVEMENT_SPEED 0.35, FOLLOW_RANGE
        // 12, MAX_HEALTH 24, ATTACK_DAMAGE 5 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.35);
        out.Register(Attribute::FollowRange,  12.0);
        out.Register(Attribute::MaxHealth,    24.0);
        out.Register(Attribute::AttackDamage,  5.0);
    }

    Vindicator::Vindicator(EntityLevel* level)
        : Monster(EntityTypeId::Vindicator, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void Vindicator::RegisterGoals() {
        // MC Vindicator.registerGoals, priority for priority.
        // super.registerGoals() is Raider/AbstractIllager machinery — SKIPPED
        // with the raid system, as are:
        //   3 AbstractIllager.RaiderOpenDoorGoal (raids + door-USE interact),
        //   4 Raider.HoldGroundAttackGoal (raid celebrate machinery),
        //   target 4 VindicatorJohnnyAttackGoal (armed only by the "Johnny"
        //     custom name — no custom-name system, so it could never start).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<AvoidEntityGoal>(this, kCreakingAvoid, 1,
                                                                    8.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(2, std::make_unique<VindicatorBreakDoorGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<MeleeAttackGoal>(this, 1.0, false));
        m_goalSelector.AddGoal(8, std::make_unique<RandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 8.0F) — our LookAtPlayerGoal
        // targets players only; the any-mob glance is not modelled.

        // MC 1 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers() —
        // the Raider exemption needs the raid roster; SetAlertOthers wakes
        // other vindicators, the same-species subset.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(
                                        this, /*mustSee=*/true));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kVillagerTargets, 2, /*mustSee=*/true));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kIronGolemTargets, 1, /*mustSee=*/true));
    }

    std::shared_ptr<SpawnGroupData>
    Vindicator::FinalizeSpawn(SpawnReason reason,
                              std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Monster::FinalizeSpawn(reason, std::move(groupData));
        // MC Vindicator.finalizeSpawn: getNavigation().setCanOpenDoors(true).
        // MC's customServerAiStep then re-gates the flag on isRaided() every
        // tick — SKIPPED (DEVIATION, see the class comment): with no raid
        // system that override would immediately kill the door pathing, so
        // the finalizeSpawn value stands.
        GetNavigation().SetCanOpenDoors(true);
        // The iron-axe equipment roll — no mob equipment system.
        return groupData;
    }

    // ── Illusioner ─────────────────────────────────────────────────────────

    void Illusioner::CreateAttributes(AttributeMap& out) {
        // MC Illusioner.createAttributes: MOVEMENT_SPEED 0.5, FOLLOW_RANGE 18,
        // MAX_HEALTH 32 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.5);
        out.Register(Attribute::FollowRange,  18.0);
        out.Register(Attribute::MaxHealth,    32.0);
    }

    Illusioner::Illusioner(EntityLevel* level)
        : SpellcasterIllager(EntityTypeId::Illusioner, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC xpReward 5 — ours comes from the entity type table.
        // MC finalizeSpawn's setItemSlot(MAINHAND, BOW) — no mob equipment
        // system; RangedBowAttackGoal treats the bow as permanently held (the
        // skeleton precedent, which is also MC's steady state).
        RegisterGoals();
    }

    void Illusioner::RegisterGoals() {
        // MC Illusioner.registerGoals, priority for priority.
        // super.registerGoals() is Raider/AbstractIllager machinery (raid
        // goals) — SKIPPED with the raid system.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<SpellcasterCastingSpellGoal>(this));
        m_goalSelector.AddGoal(3, std::make_unique<AvoidEntityGoal>(this, kCreakingAvoid, 1,
                                                                    8.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(4, std::make_unique<IllusionerMirrorSpellGoal>(this));
        m_goalSelector.AddGoal(5, std::make_unique<IllusionerBlindnessSpellGoal>(this));
        m_goalSelector.AddGoal(6, std::make_unique<RangedBowAttackGoal>(
                                      this, this, 0.5, 20, 15.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 8.0F) — our LookAtPlayerGoal
        // targets players only; the any-mob glance is not modelled.

        // MC 1 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers() —
        // the Raider exemption needs the raid roster; SetAlertOthers wakes
        // other illusioners, the same-species subset.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        // MC: player target with a 300-tick unseen memory, villagers and the
        // golem likewise (the illusioner's golem goal carries the memory too,
        // unlike the evoker's).
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoalWithMemory>(
                                        this, /*mustSee=*/true, 300));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoalWithMemory>(
                                        this, kVillagerTargets, 2,
                                        /*mustSee=*/false, 300));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoalWithMemory>(
                                        this, kIronGolemTargets, 1,
                                        /*mustSee=*/false, 300));
    }

    void Illusioner::PerformRangedAttack(LivingEntity& target, float power) {
        // MC Illusioner.performRangedAttack — the skeleton's arrow math with
        // the illusioner as shooter.
        if (!m_level) return;

        auto arrow = std::make_unique<Arrow>(m_level);
        arrow->SetOwner(this);
        arrow->position = glm::dvec3(position.x, GetEyeY() - 0.1, position.z);
        arrow->SetBaseDamageFromMob(power);

        const double xd = target.position.x - position.x;
        const double yd = (target.position.y + target.GetBbHeight() / 3.0) -
                          arrow->position.y;
        const double zd = target.position.z - position.z;
        const double horiz = std::sqrt(xd * xd + zd * zd);

        const int difficultyId = static_cast<int>(m_level->GetDifficulty());
        arrow->Shoot(xd, yd + horiz * 0.2, zd, 1.6f,
                     static_cast<float>(14 - difficultyId * 4));

        m_level->AddFreshEntity(std::move(arrow));
    }

    // ── Vex ────────────────────────────────────────────────────────────────

    void Vex::CreateAttributes(AttributeMap& out) {
        // MC Vex.createAttributes: MAX_HEALTH 14, ATTACK_DAMAGE 4 on the
        // monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    14.0);
        out.Register(Attribute::AttackDamage,  4.0);
    }

    Vex::Vex(EntityLevel* level) : Monster(EntityTypeId::Vex, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: the impulse move control; xpReward 3 (ours comes
        // from the entity type table).
        SetMoveControl(std::make_unique<VexMoveControl>(this));
        RegisterGoals();
    }

    void Vex::Tick() {
        // MC wraps super.tick() in noPhysics = true so a vex ghosts through
        // walls — the mover has no ghost mode, so a vex here respects
        // collision. DEVIATION, visible when a target hides indoors.
        Monster::Tick();
        SetNoGravity(true);

        // MC: once the limited life expires, 1 starvation damage every 20
        // ticks until the vex dies. (No STARVE source in the enum — Generic
        // carries it; both bypass nothing a vex has.)
        if (m_level && !m_level->IsClientSide() && m_hasLimitedLife &&
            --m_limitedLifeTicks <= 0) {
            m_limitedLifeTicks = 20;
            Hurt(MobDamageSource::Generic, 1.0f, nullptr);
        }
    }

    void Vex::RegisterGoals() {
        // MC Vex.registerGoals (super.registerGoals() is Monster's, which
        // registers nothing).
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(4, std::make_unique<VexChargeAttackGoal>(this));
        m_goalSelector.AddGoal(8, std::make_unique<VexRandomMoveGoal>(this));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 8.0F) — players only here.

        // MC 1 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers() —
        // Raider exemption skipped with raids.
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<VexCopyOwnerTargetGoal>(this));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackablePlayerGoal>(this, true));
    }

    // ── Wither ─────────────────────────────────────────────────────────

    void Wither::CreateAttributes(AttributeMap& out) {
        // MC Wither.createAttributes: MAX_HEALTH 300, MOVEMENT_SPEED 0.6,
        // FLYING_SPEED 0.6, FOLLOW_RANGE 40, ARMOR 4 on the monster base.
        CreateMonsterAttributes(out);
        out.Register(Attribute::MaxHealth,    300.0);
        out.Register(Attribute::MovementSpeed,  0.6);
        out.Register(Attribute::FlyingSpeed,    0.6);
        out.Register(Attribute::FollowRange,   40.0);
        out.Register(Attribute::Armor,          4.0);
    }

    Wither::Wither(EntityLevel* level)
        : Monster(EntityTypeId::Wither, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: FlyingMoveControl(10, false), the flying
        // navigation (canOpenDoors false / canFloat true have no analogue on
        // this port's flying navigation), xpReward 50 (type table), and the
        // boss bar — SKIPPED: no boss bar UI exists.
        SetMoveControl(std::make_unique<FlyingMoveControl>(this, 10, false));
        SetNavigation(std::make_unique<FlyingPathNavigation>(this, level));
        RegisterGoals();
    }

    void Wither::RegisterGoals() {
        // MC Wither.registerGoals, priority for priority.
        m_goalSelector.AddGoal(0, std::make_unique<WitherDoNothingGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<RangedAttackGoal>(
                                      this, this, 1.0, 40, 20.0f));
        m_goalSelector.AddGoal(5, std::make_unique<WaterAvoidingRandomFlyingGoal>(this, 1.0));
        m_goalSelector.AddGoal(6, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));

        // MC: HurtByTargetGoal(this, new Class[0]) — no exemptions, no alert.
        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        m_targetSelector.AddGoal(2, std::make_unique<WitherTargetGoal>(this));
    }

    void Wither::MakeInvulnerable() {
        // MC makeInvulnerable — called by the soul-sand ritual; here by
        // FinalizeSpawn (the /summon-and-egg stand-in for the ritual).
        SetInvulnerableTicks(220);
        SetHealth(GetMaxHealth() / 3.0f);
    }

    std::shared_ptr<SpawnGroupData>
    Wither::FinalizeSpawn(SpawnReason reason,
                              std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Monster::FinalizeSpawn(reason, std::move(groupData));
        // MC's WitherSkullBlock ritual calls makeInvulnerable() on the wither
        // it builds; the egg//summon path is this port's only spawn ritual,
        // so it charges up the same way. (MC's bare /summon skips this.)
        if (reason != SpawnReason::Load) {
            MakeInvulnerable();
        }
        return groupData;
    }

    void Wither::AiStep() {
        // MC Wither.aiStep head, transcribed: damp vertical motion to
        // 60%, then chase the PRIMARY target (alternativeTarget(0), which
        // customServerAiStep keeps equal to getTarget()) — climb while below
        // it (or, unpowered, below its head + 5) and drift horizontally
        // toward it beyond 3 blocks.
        glm::dvec3 dm(velocity.x, velocity.y * 0.6, velocity.z);
        if (m_level && !m_level->IsClientSide() && GetTarget() != nullptr) {
            const Entity* entity = GetTarget();
            double yd = dm.y;
            if (position.y < entity->position.y ||
                (!IsPowered() && position.y < entity->position.y + 5.0)) {
                yd = std::max(0.0, yd);
                yd += 0.3 - yd * 0.6;
            }
            dm.y = yd;

            const glm::dvec3 delta(entity->position.x - position.x, 0.0,
                                   entity->position.z - position.z);
            const double horizSq = delta.x * delta.x + delta.z * delta.z;
            if (horizSq > 9.0) {
                const glm::dvec3 scale = delta / std::sqrt(horizSq);
                dm.x += scale.x * 0.3 - dm.x * 0.6;
                dm.z += scale.z * 0.3 - dm.z * 0.6;
            }
        }
        velocity = dm;
        if (dm.x * dm.x + dm.z * dm.z > 0.05) {
            yRot = static_cast<float>(std::atan2(dm.z, dm.x)) * Mth::kRadToDeg -
                   90.0f;
        }

        Monster::AiStep();

        // MC's tail: the side-head visual aim (xRotHeads/yRotHeads lerped at
        // the per-head targets) — SKIPPED: the model renders the small heads
        // forward (their yaws are unported model state, and syncing two
        // extra rotation pairs is not worth it before the model reads them).

        // MC Wither.aiStep's particle tail, verbatim. MC runs it on both
        // sides (addParticle no-ops on the server); gated client-side here
        // so the server skips the RNG draws it would throw away. getScale()
        // is 1 — no mob-scale attribute in this port.
        if (m_level && m_level->IsClientSide()) {
            JavaRandom& rng = m_level->Random();
            const bool isPowered = m_clientPowered;   // synced half of MC isPowered()
            for (int i = 0; i < 3; ++i) {
                const double hx = GetHeadX(i);
                const double hy = GetHeadY(i);
                const double hz = GetHeadZ(i);
                const float radius = 0.3f;   // 0.3F * getScale()
                m_level->AddParticle(ParticleKind::Smoke,
                                     hx + rng.NextGaussian() * radius,
                                     hy + rng.NextGaussian() * radius,
                                     hz + rng.NextGaussian() * radius,
                                     0.0, 0.0, 0.0);
                if (isPowered && rng.NextInt(4) == 0) {
                    // ColorParticleOption.create(ENTITY_EFFECT, 0.7, 0.7, 0.5)
                    m_level->AddColorParticle(ParticleKind::EntityEffect,
                                              hx + rng.NextGaussian() * radius,
                                              hy + rng.NextGaussian() * radius,
                                              hz + rng.NextGaussian() * radius,
                                              0.0, 0.0, 0.0,
                                              0.7f, 0.7f, 0.5f, 1.0f);
                }
            }
            // The spawn charge-up column (getInvulnerableTicks() > 0 — the
            // synced bit here).
            if (m_clientInvulnerable) {
                const float height = 3.3f;   // 3.3F * getScale()
                for (int i = 0; i < 3; ++i) {
                    // ColorParticleOption.create(ENTITY_EFFECT, 0.7, 0.7, 0.9)
                    m_level->AddColorParticle(ParticleKind::EntityEffect,
                                              position.x + rng.NextGaussian(),
                                              position.y + static_cast<double>(
                                                  rng.NextFloat() * height),
                                              position.z + rng.NextGaussian(),
                                              0.0, 0.0, 0.0,
                                              0.7f, 0.7f, 0.9f, 1.0f);
                }
            }
        }
    }

    void Wither::CustomServerAiStep() {
        if (!m_level) return;
        JavaRandom& rng = m_level->Random();

        if (m_invulnerableTicks > 0) {
            // ── The 220-tick spawn charge ─────────────────────────────────
            const int newCount = m_invulnerableTicks - 1;
            // MC drives the boss bar progress here — no boss bar UI.
            if (newCount <= 0) {
                // MC: level.explode(this, x, eyeY, z, 7.0F, false, MOB) —
                // entity-damage-only here per project policy (creeper
                // precedent: no explosion system, terrain untouched), and
                // global level event 1023 (the spawn sound) — no sound
                // system.
                SpawnBurstExplosion();
            }
            SetInvulnerableTicks(newCount);
            if (tickCount % 10 == 0) {
                Heal(10.0f);
            }
            return;
        }

        Monster::CustomServerAiStep();

        // ── The side heads (MC's i = 1..2 loop) ───────────────────────────
        for (int i = 1; i < 3; ++i) {
            if (tickCount < m_nextHeadUpdate[i - 1]) continue;
            m_nextHeadUpdate[i - 1] = tickCount + 10 + rng.NextInt(10);

            // MC: on NORMAL/HARD, a head idle for more than 15 updates fires
            // a dangerous skull at a random point in a 20x10x20 box.
            if (m_level->GetDifficulty() == Difficulty::Normal ||
                m_level->GetDifficulty() == Difficulty::Hard) {
                const int idle = m_idleHeadUpdates[i - 1]++;
                if (idle > 15) {
                    const double xt =
                        position.x - 10.0 + rng.NextDouble() * 20.0;
                    const double yt =
                        position.y - 5.0 + rng.NextDouble() * 10.0;
                    const double zt =
                        position.z - 10.0 + rng.NextDouble() * 20.0;
                    PerformRangedAttack(i + 1, xt, yt, zt, true);
                    m_idleHeadUpdates[i - 1] = 0;
                }
            }

            LivingEntity* headTarget = m_headTargets[i - 1];
            if (headTarget != nullptr) {
                if (headTarget->IsAlive() && CanAttack(*headTarget) &&
                    DistanceToSqr(*headTarget) <= 900.0 &&
                    GetSensing().HasLineOfSight(*headTarget)) {
                    PerformRangedAttack(i + 1, *headTarget);
                    m_nextHeadUpdate[i - 1] = tickCount + 40 + rng.NextInt(20);
                    m_idleHeadUpdates[i - 1] = 0;
                } else {
                    m_headTargets[i - 1] = nullptr;
                }
            } else {
                // MC: pick a RANDOM valid living entity within the
                // (20, 8, 20)-inflated box — same selector as the target
                // goal (attackable, not a WITHER_FRIEND/undead).
                std::vector<LivingEntity*> candidates;
                auto consider = [&](LivingEntity* living) {
                    if (!living || !living->IsAlive()) return;
                    if (IsUndeadEntityType(living->GetType())) return;
                    if (dynamic_cast<const Projectile*>(living)) return;
                    if (DistanceToSqr(*living) > 20.0 * 20.0) return;
                    if (!living->IsAttackable()) return;
                    candidates.push_back(living);
                };

                AABB box = GetAABB();
                box.min -= glm::vec3(20.0f, 8.0f, 20.0f);
                box.max += glm::vec3(20.0f, 8.0f, 20.0f);
                std::vector<Entity*> nearby;
                m_level->GetEntitiesInBox(box, this, nearby);
                for (Entity* e : nearby) {
                    consider(dynamic_cast<LivingEntity*>(e));
                }
                std::vector<LivingEntity*> players;
                m_level->GetPlayers(players);
                for (LivingEntity* player : players) {
                    if (player && player->GetAABB().Intersects(box)) {
                        consider(player);
                    }
                }

                if (!candidates.empty()) {
                    m_headTargets[i - 1] = candidates[rng.NextInt(
                        static_cast<int>(candidates.size()))];
                }
            }
        }

        // MC syncs alternativeTarget(0) = getTarget()'s id here; the primary
        // target is read straight off GetTarget() in this port (the synced
        // id only feeds the skipped beam-of-heads client visuals).

        // MC destroyBlocksTick: armed by Hurt, and at 0 (with mobGriefing)
        // the wither smashes every non-WITHER_IMMUNE block its box overlaps.
        // Block destruction by mobs is not modelled — the timer runs so the
        // wiring reads like MC's, the smash itself is the skipped piece.
        if (m_destroyBlocksTick > 0) {
            --m_destroyBlocksTick;
        }

        // MC: 1 HP of regeneration every second.
        if (tickCount % 20 == 0) {
            Heal(1.0f);
        }

        // MC updates the boss bar progress here — no boss bar UI.
    }

    double Wither::GetHeadX(int head) const {
        if (head <= 0) return position.x;
        const float angle =
            (yBodyRot + static_cast<float>(180 * (head - 1))) * Mth::kDegToRad;
        return position.x + static_cast<double>(std::cos(angle)) * 1.3;
    }

    double Wither::GetHeadY(int head) const {
        return position.y + (head <= 0 ? 3.0 : 2.2);
    }

    double Wither::GetHeadZ(int head) const {
        if (head <= 0) return position.z;
        const float angle =
            (yBodyRot + static_cast<float>(180 * (head - 1))) * Mth::kDegToRad;
        return position.z + static_cast<double>(std::sin(angle)) * 1.3;
    }

    void Wither::PerformRangedAttack(int head, LivingEntity& target) {
        // MC: aim half an eye-height up the target; the 0.1% dangerous roll
        // exists only for head 0 (short-circuited for the side heads, so the
        // RNG stream matches).
        const bool dangerous =
            head == 0 && m_level && m_level->Random().NextFloat() < 0.001f;
        PerformRangedAttack(head, target.position.x,
                            target.position.y +
                                static_cast<double>(target.GetEyeHeight()) * 0.5,
                            target.position.z, dangerous);
    }

    void Wither::PerformRangedAttack(int head, double tx, double ty,
                                         double tz, bool dangerous) {
        if (!m_level) return;
        // MC level event 1024 (wither shoot sound) — no sound system.

        const double hx = GetHeadX(head);
        const double hy = GetHeadY(head);
        const double hz = GetHeadZ(head);

        auto skull = std::make_unique<WitherSkull>(m_level);
        skull->SetOwnerAndDirection(*this, glm::dvec3(tx - hx, ty - hy, tz - hz));
        if (dangerous) {
            skull->SetDangerous(true);
        }
        skull->position = glm::dvec3(hx, hy, hz);
        m_level->AddFreshEntity(std::move(skull));
    }

    void Wither::PerformRangedAttack(LivingEntity& target, float power) {
        // MC RangedAttackMob.performRangedAttack — the centre head's volley,
        // driven by RangedAttackGoal(1.0, 40, 20).
        (void)power;
        PerformRangedAttack(0, target);
    }

    void Wither::SpawnBurstExplosion() {
        // MC Wither's spawn charge: level.explode(this, getX, getEyeY, getZ,
        // 7.0F, false, ExplosionInteraction.MOB) — centred at the EYES, which
        // is what lifts the crater clear of the summoning platform.
        if (!m_level) return;
        ExplosionParams params;
        params.center       = glm::dvec3(position.x, GetEyeY(), position.z);
        params.radius       = 7.0f;
        params.source       = this;
        params.attributedTo = this;
        params.interaction  = ExplosionInteraction::Mob;
        // Qualified: Creeper::Explode / the member we are inside shadows the
        // free function.
        Game::Explode(*m_level, params);
    }

    void Wither::HandleEntityEvent(uint8_t id) {
        // See Creeper::HandleEntityEvent — the burst visual is a packet now.
        Monster::HandleEntityEvent(id);
    }

    bool Wither::Hurt(MobDamageSource source, float amount,
                          Entity* attacker) {
        // MC hurtServer's immunity ladder, in order:
        // DamageTypeTags.WITHER_IMMUNE_TO is drowning + dragon breath —
        // Drown is the one this engine can produce.
        if (source == MobDamageSource::Drown) return false;
        // A wither never hurts a wither (its own skulls included — the
        // skull's damage attribution is its owner).
        if (attacker != nullptr && attacker->GetType() == EntityTypeId::Wither) {
            return false;
        }
        // The spawn charge shrugs everything off (MC excepts only
        // BYPASSES_INVULNERABILITY — /kill — which has no damage source
        // here).
        if (m_invulnerableTicks > 0) return false;
        // Powered (half health): arrows and wind charges bounce. The wire
        // does not say WHICH projectile hit, so every projectile bounces —
        // same approximation the shulker documents (MC would let a shulker
        // bullet or trident... tridents are AbstractArrows and bounce too;
        // only the shulker bullet differs).
        if (IsPowered() && source == MobDamageSource::Projectile) return false;
        // MC EntityTypeTags.WITHER_FRIENDS (#undead) never hurt it.
        if (auto* living = dynamic_cast<LivingEntity*>(attacker)) {
            if (IsUndeadEntityType(living->GetType())) return false;
        }

        // A landed hit arms the block-smash timer and hurries every idle
        // head toward its next volley.
        if (m_destroyBlocksTick <= 0) {
            m_destroyBlocksTick = 20;
        }
        for (int& idle : m_idleHeadUpdates) {
            idle += 3;
        }

        return Monster::Hurt(source, amount, attacker);
    }

    void Wither::CheckDespawn() {
        // MC Wither.checkDespawn: a wither NEVER despawns by distance —
        // peaceful discards it, anything else just resets the idle clock.
        if (!m_level || m_level->IsClientSide()) return;
        if (m_level->GetDifficulty() == Difficulty::Peaceful &&
            TypeInfo().notInPeaceful) {
            Discard();
            return;
        }
        m_noActionTime = 0;
    }

    void Wither::DropCustomDeathLoot(EntityLevel& level) {
        // MC dropCustomDeathLoot: the nether star (setExtendedLifetime has no
        // analogue — item entities here have one lifetime).
        level.SpawnItemDrop(position, Items::NetherStar, 1);
    }

    void Wither::ClearReferenceTo(const Entity* entity) {
        Monster::ClearReferenceTo(entity);
        for (LivingEntity*& target : m_headTargets) {
            if (target == entity) target = nullptr;
        }
    }

    // ── Strider ────────────────────────────────────────────────────────────

    void Strider::CreateAttributes(AttributeMap& out) {
        // MC Strider.createAttributes: Animal.createAnimalAttributes() +
        // MOVEMENT_SPEED 0.175.
        CreateAnimalAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.175);
    }

    Strider::Strider(EntityLevel* level) : Animal(EntityTypeId::Strider, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor maluses: water forbidden, lava and fire free — a
        // strider lives on lava. (blocksBuilding and the item steering have
        // no analogue.)
        SetPathfindingMalus(PathType::Water, -1.0f);
        SetPathfindingMalus(PathType::Lava, 0.0f);
        SetPathfindingMalus(PathType::DangerFire, 0.0f);
        SetPathfindingMalus(PathType::DamageFire, 0.0f);
        RegisterGoals();
    }

    void Strider::RegisterGoals() {
        // MC Strider.registerGoals, priority for priority:
        //   3 TemptGoal(1.4, STRIDER_TEMPT_ITEMS, false) — SKIPPED: no warped
        //     fungus item exists (IsFood is empty for the same reason, which
        //     also parks BreedGoal — registered so the table reads like MC's,
        //     it simply never finds a lover without food).
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 1.65));
        m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
        m_goalSelector.AddGoal(4, std::make_unique<StriderGoToLavaGoal>(this, 1.0));
        m_goalSelector.AddGoal(5, std::make_unique<FollowParentGoal>(this, 1.0));
        m_goalSelector.AddGoal(7, std::make_unique<RandomStrollGoal>(this, 1.0, 60));
        m_goalSelector.AddGoal(8, std::make_unique<LookAtPlayerGoal>(this, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));
        // MC 9 LookAtPlayerGoal(Strider.class, 8.0F) — our LookAtPlayerGoal
        // targets players only; the strider-watching glance is not modelled.
    }

    void Strider::SetSuffocating(bool v) {
        if (m_suffocating == v) return;
        m_suffocating = v;
        // MC SUFFOCATING_MODIFIER: -0.34 ADD_MULTIPLIED_BASE on
        // MOVEMENT_SPEED while cold. Applied by re-basing (the Ravager
        // precedent) — no per-mob modifier id burned for one flag.
        m_attributes.SetBaseValue(Attribute::MovementSpeed,
                                  v ? 0.175 * (1.0 - 0.34) : 0.175);
        needsSync = true;
    }

    bool Strider::IsInLava() const {
        // The engine's Entity::IsInLava has no fluid tracking; sample the
        // feet cell (and the body cell for a tall strider mid-sink).
        if (!m_level || !m_level->Blocks()) return false;
        const IBlockAccess& blocks = *m_level->Blocks();
        const glm::ivec3 p = BlockPosition();
        return blocks.GetBlock(p.x, p.y, p.z) == BlockID::Lava ||
               blocks.GetBlock(p.x, p.y + 1, p.z) == BlockID::Lava;
    }

    float Strider::GetWalkTargetValue(const glm::ivec3& pos) const {
        // MC: lava scores 10; while IN lava everything else is -infinity (a
        // strider already home never rates dry land); ashore it is neutral.
        if (m_level && m_level->Blocks() &&
            m_level->Blocks()->GetBlock(pos.x, pos.y, pos.z) == BlockID::Lava) {
            return 10.0f;
        }
        return IsInLava() ? -std::numeric_limits<float>::infinity() : 0.0f;
    }

    bool Strider::CheckSpawnObstruction(EntityLevel& level) const {
        // MC Strider.checkSpawnObstruction: level.isUnobstructed(this) ONLY —
        // the base's no-liquid test would reject every lava spawn. Same
        // reduction as the guardian's.
        const AABB box = GetAABB();
        std::vector<Entity*> occupants;
        level.GetEntitiesInBox(box, this, occupants);
        for (const Entity* other : occupants) {
            if (!other->IsRemoved() && other->GetAABB().Intersects(box)) return false;
        }
        return true;
    }

    void Strider::Tick() {
        // MC Strider.tick: the happy/retreat sound rolls need the sound
        // system; then the warm-block check drives the shiver.
        if (!IsNoAi()) {
            // MC STRIDER_WARM_BLOCKS is lava (+ magma in datapacks); with no
            // block tags the check reduces to lava contact — feet cell,
            // stand-on cell, or any lava fluid height. (The
            // riding-a-suffocating-strider propagation goes with strider
            // stacking, which the jockey spawn can produce — propagated
            // below.)
            bool inWarmBlocks = false;
            if (m_level && m_level->Blocks()) {
                const IBlockAccess& blocks = *m_level->Blocks();
                const glm::ivec3 p = BlockPosition();
                inWarmBlocks =
                    blocks.GetBlock(p.x, p.y, p.z) == BlockID::Lava ||
                    blocks.GetBlock(p.x, p.y - 1, p.z) == BlockID::Lava;
            }
            bool vehicleSuffocating = false;
            if (const auto* vehicle = dynamic_cast<const Strider*>(GetVehicle())) {
                vehicleSuffocating = vehicle->IsSuffocating();
            }
            if (m_level && !m_level->IsClientSide()) {
                SetSuffocating(!inWarmBlocks || vehicleSuffocating);
            }
        }

        Animal::Tick();
        FloatStrider();
    }

    void Strider::FloatStrider() {
        // MC floatStrider: submerged in lava the strider bobs upward
        // (velocity halved plus 0.05 up); at the surface MC's fluid-standing
        // collision (canStandOnFluid) takes over — this port's mover has no
        // fluid support, so the surface half lives in Travel below.
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const glm::ivec3 p = BlockPosition();
        if (blocks.GetBlock(p.x, p.y, p.z) == BlockID::Lava) {
            const bool aboveFree =
                blocks.GetBlock(p.x, p.y + 1, p.z) != BlockID::Lava;
            const double fraction = position.y - std::floor(position.y);
            if (aboveFree && fraction > 0.9 && velocity.y <= 0.0) {
                // Riding the surface: snap onto the cell top and stand.
                position.y = static_cast<double>(p.y) + 1.0;
                velocity.y = 0.0;
                onGround = true;
                needsSync = true;
            } else {
                velocity = velocity * 0.5 + glm::dvec3(0.0, 0.05, 0.0);
                needsSync = true;
            }
        }
    }

    void Strider::Travel(const glm::dvec3& input) {
        Animal::Travel(input);

        // LAVA-WALK SUPPORT (documented approximation): MC's
        // canStandOnFluid(LAVA) makes the mover collide with the lava
        // surface. This mover knows no fluids, so after the ordinary travel
        // a strider standing over lava is caught and held on the cell top —
        // the observable result (walks on lava, never sinks while alive)
        // matches; the ~0.1-block surface offset MC renders does not.
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const glm::ivec3 p = BlockPosition();
        if (blocks.GetBlock(p.x, p.y, p.z) != BlockID::Lava &&
            blocks.GetBlock(p.x, p.y - 1, p.z) == BlockID::Lava &&
            velocity.y <= 0.0) {
            position.y = static_cast<double>(p.y);
            velocity.y = 0.0;
            onGround = true;
        }
    }

    std::shared_ptr<SpawnGroupData>
    Strider::FinalizeSpawn(SpawnReason reason,
                           std::shared_ptr<SpawnGroupData> groupData) {
        // MC Strider.finalizeSpawn — babies skip every roll.
        if (IsBaby() || !m_level) {
            return Animal::FinalizeSpawn(reason, std::move(groupData));
        }
        JavaRandom& rng = m_level->Random();

        if (rng.NextInt(30) == 0) {
            // MC: a zombified-piglin jockey with a warped fungus on a stick
            // and a saddled strider (both item halves wait on mob equipment;
            // the RIDING half is live). The jockey's group token pins the
            // pack's baby roll and forbids chicken jockeys, exactly as MC
            // passes ZombieGroupData(getSpawnAsBabyOdds(random), false).
            auto jockey = std::make_unique<ZombifiedPiglin>(m_level);
            jockey->position = position;
            jockey->yRot = jockey->yBodyRot = jockey->yHeadRot = yRot;
            jockey->FinalizeSpawn(
                SpawnReason::Jockey,
                std::make_shared<ZombieGroupData>(rng.NextFloat() < 0.05f, false));
            ZombifiedPiglin* placed = jockey.get();
            m_level->AddFreshEntity(std::move(jockey));
            placed->StartRiding(*this, /*force=*/true);
            groupData = std::make_shared<StriderGroupData>(0.0f);
        } else if (rng.NextInt(10) == 0) {
            // MC: a baby strider riding the adult.
            auto jockey = std::make_unique<Strider>(m_level);
            jockey->SetAge(kBabyStartAge);
            jockey->position = position;
            jockey->yRot = jockey->yBodyRot = jockey->yHeadRot = yRot;
            jockey->FinalizeSpawn(SpawnReason::Jockey, nullptr);
            Strider* placed = jockey.get();
            m_level->AddFreshEntity(std::move(jockey));
            placed->StartRiding(*this, /*force=*/true);
            groupData = std::make_shared<StriderGroupData>(0.0f);
        } else if (!groupData) {
            groupData = std::make_shared<StriderGroupData>(0.5f);
        }

        // MC AgeableMob.finalizeSpawn consumes the token (a babyChance roll
        // per pack member); our AgeableMob has no finalizeSpawn, so the
        // consumption is inlined here.
        if (auto* data = dynamic_cast<StriderGroupData*>(groupData.get())) {
            if (data->babyChance > 0.0f && rng.NextFloat() < data->babyChance) {
                SetAge(kBabyStartAge);
            }
        }

        return Animal::FinalizeSpawn(reason, std::move(groupData));
    }

    // ── Ender dragon ───────────────────────────────────────────────────────

    void DragonFlightHistory::Record(double y, float yRot) {
        // MC DragonFlightHistory.record — the first sample floods the ring.
        const Sample sample{y, yRot};
        if (head < 0) {
            for (Sample& s : samples) s = sample;
        }
        if (++head == kLength) head = 0;
        samples[head] = sample;
    }

    DragonFlightHistory::Sample DragonFlightHistory::Get(int delay,
                                                         float partialTick) const {
        const Sample sample = Get(delay);
        const Sample sampleOld = Get(delay + 1);
        return Sample{
            Mth::Lerp(static_cast<double>(partialTick), sampleOld.y, sample.y),
            Mth::RotLerp(partialTick, sampleOld.yRot, sample.yRot)};
    }

    float DragonPhaseInstance::GetTurnSpeed() const {
        // MC AbstractDragonPhaseInstance.getTurnSpeed.
        const double hx = m_dragon->velocity.x;
        const double hz = m_dragon->velocity.z;
        const float rotSpeed =
            static_cast<float>(std::sqrt(hx * hx + hz * hz)) + 1.0f;
        const float dist = std::min(rotSpeed, 40.0f);
        return 0.7f / dist / rotSpeed;
    }

    namespace {

        // MC BlockTags.DRAGON_IMMUNE (data/minecraft/tags/block/
        // dragon_immune.json, vendored under data/) — the blocks the dragon
        // cannot smash through.
        bool IsDragonImmuneBlock(BlockID id) {
            switch (id) {
                case BlockID::Barrier:
                case BlockID::Bedrock:
                case BlockID::EndPortal:
                case BlockID::EndPortalFrame:
                case BlockID::EndGateway:
                case BlockID::CommandBlock:
                case BlockID::RepeatingCommandBlock:
                case BlockID::ChainCommandBlock:
                case BlockID::StructureBlock:
                case BlockID::Jigsaw:
                case BlockID::MovingPiston:
                case BlockID::Obsidian:
                case BlockID::CryingObsidian:
                case BlockID::EndStone:
                case BlockID::IronBars:
                case BlockID::RespawnAnchor:
                case BlockID::ReinforcedDeepslate:
                    return true;
                default:
                    return false;
            }
        }

        // MC BlockTags.DRAGON_TRANSPARENT — light + #fire: blocks that
        // neither stop the dragon nor get smashed.
        bool IsDragonTransparentBlock(BlockID id) {
            return id == BlockID::Air || id == BlockID::Light ||
                   id == BlockID::Fire || id == BlockID::SoulFire;
        }

        // MC Level.getHeightmapPos(MOTION_BLOCKING_NO_LEAVES, ...) stand-in —
        // the first free cell above the highest motion-blocking-or-fluid
        // block (MC's MOTION_BLOCKING heightmaps count fluids). Column walk;
        // no heightmap lives on IBlockAccess (the phantom's precedent).
        int MotionBlockingY(const IBlockAccess& blocks, int x, int z) {
            for (int y = 319; y >= -64; --y) {
                if (BlockRegistry::HasCollision(blocks.GetBlock(x, y, z)) ||
                    blocks.IsBlockFluid(x, y, z)) {
                    return y + 1;
                }
            }
            return -64;
        }

        glm::dvec3 SafeNormalize(const glm::dvec3& v) {
            // MC Vec3.normalize: the zero vector normalises to zero.
            const double len = glm::length(v);
            return len > 1.0e-8 ? v / len : glm::dvec3(0.0);
        }

        // MC EnderDragonPart boxes are centred on the part position with a
        // given width/height; the part position is its FEET.
        AABB PartBox(const glm::dvec3& feet, float width, float height) {
            return AABB(glm::vec3(feet.x, feet.y + height * 0.5f, feet.z),
                        glm::vec3(width, height, width));
        }

        // ── The phase instances (MC boss/enderdragon/phases) ───────────────

        class DragonHoldingPatternPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            DragonPhase GetPhase() const override {
                return DragonPhase::HoldingPattern;
            }

            void Begin() override {
                m_hasPath = false;
                m_hasTarget = false;
            }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void DoServerTick() override;

        private:
            void FindNewTarget();
            void NavigateToNextPathNode();

            EnderDragon::FlightPath m_currentPath;
            bool       m_hasPath = false;
            glm::dvec3 m_targetLocation{0.0};
            bool       m_hasTarget = false;
            bool       m_clockwise = false;
        };

        class DragonStrafePlayerPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            static constexpr int kFireballChargeAmount = 5;   // MC

            DragonPhase GetPhase() const override {
                return DragonPhase::StrafePlayer;
            }

            void Begin() override {
                m_fireballCharge = 0;
                m_hasTarget = false;
                m_hasPath = false;
                m_attackTarget = nullptr;
            }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void SetTarget(LivingEntity* target);
            void DoServerTick() override;

            void ClearReferenceTo(const Entity* entity) override {
                if (m_attackTarget == entity) m_attackTarget = nullptr;
            }

        private:
            void FindNewTarget();
            void NavigateToNextPathNode();

            int  m_fireballCharge = 0;
            EnderDragon::FlightPath m_currentPath;
            bool       m_hasPath = false;
            glm::dvec3 m_targetLocation{0.0};
            bool       m_hasTarget = false;
            LivingEntity* m_attackTarget = nullptr;
            bool       m_holdingPatternClockwise = false;
        };

        class DragonTakeoffPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            DragonPhase GetPhase() const override { return DragonPhase::Takeoff; }

            void Begin() override {
                m_firstTick = true;
                m_hasPath = false;
                m_hasTarget = false;
            }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void DoServerTick() override;

        private:
            void FindNewTarget();
            void NavigateToNextPathNode();

            bool m_firstTick = false;
            EnderDragon::FlightPath m_currentPath;
            bool       m_hasPath = false;
            glm::dvec3 m_targetLocation{0.0};
            bool       m_hasTarget = false;
        };

        // MC DragonChargePlayerPhase. Complete, but note its ONLY vanilla
        // trigger is DragonSittingScanningPhase — the skipped perch cycle —
        // so nothing reaches it yet; it is here for when that cycle lands.
        class DragonChargePlayerPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            static constexpr int kChargeRecoveryTime = 10;   // MC

            DragonPhase GetPhase() const override {
                return DragonPhase::ChargingPlayer;
            }

            void Begin() override {
                m_hasTarget = false;
                m_timeSinceCharge = 0;
            }

            void SetTarget(const glm::dvec3& target) {
                m_targetLocation = target;
                m_hasTarget = true;
            }

            float GetFlySpeed() const override { return 3.0f; }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void DoServerTick() override;

        private:
            glm::dvec3 m_targetLocation{0.0};
            bool m_hasTarget = false;
            int  m_timeSinceCharge = 0;
        };

        class DragonDeathPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            DragonPhase GetPhase() const override { return DragonPhase::Dying; }

            void Begin() override {
                m_hasTarget = false;
                m_time = 0;
            }

            float GetFlySpeed() const override { return 3.0f; }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void DoServerTick() override;

        private:
            glm::dvec3 m_targetLocation{0.0};
            bool m_hasTarget = false;
            int  m_time = 0;
        };

        class DragonHoverPhase : public DragonPhaseInstance {
        public:
            using DragonPhaseInstance::DragonPhaseInstance;

            DragonPhase GetPhase() const override { return DragonPhase::Hovering; }
            bool IsSitting() const override { return true; }
            float GetFlySpeed() const override { return 1.0f; }

            void Begin() override { m_hasTarget = false; }

            bool GetFlyTargetLocation(glm::dvec3& out) const override {
                out = m_targetLocation;
                return m_hasTarget;
            }

            void DoServerTick() override {
                if (!m_hasTarget) {
                    m_targetLocation = m_dragon->position;
                    m_hasTarget = true;
                }
            }

        private:
            glm::dvec3 m_targetLocation{0.0};
            bool m_hasTarget = false;
        };

        // ── DragonHoldingPatternPhase ──────────────────────────────────────

        void DragonHoldingPatternPhase::DoServerTick() {
            // MC DragonHoldingPatternPhase.doServerTick.
            const double distToTarget =
                !m_hasTarget ? 0.0
                             : glm::dot(m_targetLocation - m_dragon->position,
                                        m_targetLocation - m_dragon->position);
            if (distToTarget < 100.0 || distToTarget > 22500.0 ||
                m_dragon->horizontalCollision || m_dragon->verticalCollision) {
                FindNewTarget();
            }
        }

        void DragonHoldingPatternPhase::FindNewTarget() {
            EntityLevel* level = m_dragon->Level();
            if (!level) return;
            JavaRandom& rng = level->Random();

            if (m_hasPath && m_currentPath.IsDone()) {
                // MC: crystalsAlive is 0 with no dragon fight.
                // MC rand(crystals + 3) == 0 → LANDING_APPROACH: the perch
                // cycle, SKIPPED (no podium / sitting phases) — the roll is
                // drawn so the RNG stream keeps MC's shape, and the dragon
                // circles on instead of landing.
                (void)rng.NextInt(3);

                // MC: the player nearest the podium decides the strafe odds.
                const glm::ivec3 egg = m_dragon->GetPodiumPos();
                LivingEntity* player = level->GetNearestPlayer(
                    egg.x + 0.5, egg.y + 0.5, egg.z + 0.5, -1.0);
                // MC NEW_TARGET_TARGETING = forCombat().ignoreLineOfSight()
                // — creative/spectator excluded. GetNearestPlayer has no
                // filter, so the nearest player is tested afterwards
                // (approximation: MC would fall through to the next-nearest
                // valid player).
                if (player && !player->IsAttackable()) player = nullptr;

                double distSqr;
                if (player != nullptr) {
                    const double dx = egg.x + 0.5 - player->position.x;
                    const double dy = egg.y + 0.5 - player->position.y;
                    const double dz = egg.z + 0.5 - player->position.z;
                    distSqr = (dx * dx + dy * dy + dz * dz) / 512.0;
                } else {
                    distSqr = 64.0;
                }

                if (player != nullptr &&
                    (rng.NextInt(static_cast<int>(distSqr + 2.0)) == 0 ||
                     rng.NextInt(/*crystals + 2*/ 2) == 0)) {
                    // MC strafePlayer(player).
                    m_dragon->SetPhase(DragonPhase::StrafePlayer);
                    static_cast<DragonStrafePlayerPhase&>(
                        m_dragon->GetPhaseInstance(DragonPhase::StrafePlayer))
                        .SetTarget(player);
                    return;
                }
            }

            if (!m_hasPath || m_currentPath.IsDone()) {
                const int currentNodeIndex = m_dragon->FindClosestNode();
                int targetNodeIndex = currentNodeIndex;
                if (rng.NextInt(8) == 0) {
                    m_clockwise = !m_clockwise;
                    targetNodeIndex = currentNodeIndex + 6;
                }
                if (m_clockwise) ++targetNodeIndex;
                else --targetNodeIndex;

                // MC: with a dragon fight and crystals the outer ring (%12);
                // without one — this port's always — the inner 8-ring:
                targetNodeIndex -= 12;
                targetNodeIndex &= 7;
                targetNodeIndex += 12;

                m_hasPath = m_dragon->FindPath(currentNodeIndex, targetNodeIndex,
                                               nullptr, m_currentPath);
                if (m_hasPath) {
                    m_currentPath.Advance();
                }
            }

            NavigateToNextPathNode();
        }

        void DragonHoldingPatternPhase::NavigateToNextPathNode() {
            // MC navigateToNextPathNode — the node plus 0..20 random height.
            if (m_hasPath && !m_currentPath.IsDone()) {
                const glm::ivec3 current = m_currentPath.NextNodePos();
                m_currentPath.Advance();

                JavaRandom& rng = m_dragon->Level()->Random();
                double yTarget;
                do {
                    yTarget = static_cast<double>(
                        static_cast<float>(current.y) + rng.NextFloat() * 20.0f);
                } while (yTarget < static_cast<double>(current.y));

                m_targetLocation = glm::dvec3(current.x, yTarget, current.z);
                m_hasTarget = true;
            }
        }

        // ── DragonStrafePlayerPhase ────────────────────────────────────────

        void DragonStrafePlayerPhase::SetTarget(LivingEntity* target) {
            // MC setTarget — path from the current node toward the node
            // nearest the player, finished with a final node at the player's
            // strafing height.
            m_attackTarget = target;
            const int currentNodeIndex = m_dragon->FindClosestNode();
            const int targetNodeIndex = m_dragon->FindClosestNode(
                target->position.x, target->position.y, target->position.z);
            const int finalXTarget =
                static_cast<int>(std::floor(target->position.x));
            const int finalZTarget =
                static_cast<int>(std::floor(target->position.z));
            const double xd = static_cast<double>(finalXTarget) - m_dragon->position.x;
            const double zd = static_cast<double>(finalZTarget) - m_dragon->position.z;
            const double sd = std::sqrt(xd * xd + zd * zd);
            const double ho = std::min(0.4 + sd / 80.0 - 1.0, 10.0);
            const int finalYTarget =
                static_cast<int>(std::floor(target->position.y + ho));
            const glm::ivec3 finalNode(finalXTarget, finalYTarget, finalZTarget);

            m_hasPath = m_dragon->FindPath(currentNodeIndex, targetNodeIndex,
                                           &finalNode, m_currentPath);
            if (m_hasPath) {
                m_currentPath.Advance();
                NavigateToNextPathNode();
            }
        }

        void DragonStrafePlayerPhase::DoServerTick() {
            // MC DragonStrafePlayerPhase.doServerTick, transcribed.
            EntityLevel* level = m_dragon->Level();
            if (!level) return;

            if (m_attackTarget == nullptr || !m_attackTarget->IsAlive()) {
                // MC logs a warning and bails to the holding pattern.
                m_dragon->SetPhase(DragonPhase::HoldingPattern);
                return;
            }

            if (m_hasPath && m_currentPath.IsDone()) {
                // Path exhausted: hover at strafing height over the target.
                const double xTarget = m_attackTarget->position.x;
                const double zTarget = m_attackTarget->position.z;
                const double xd = xTarget - m_dragon->position.x;
                const double zd = zTarget - m_dragon->position.z;
                const double dist = std::sqrt(xd * xd + zd * zd);
                const double heightOffset = std::min(0.4 + dist / 80.0 - 1.0, 10.0);
                m_targetLocation = glm::dvec3(
                    xTarget, m_attackTarget->position.y + heightOffset, zTarget);
                m_hasTarget = true;
            }

            const double distToTarget =
                !m_hasTarget ? 0.0
                             : glm::dot(m_targetLocation - m_dragon->position,
                                        m_targetLocation - m_dragon->position);
            if (distToTarget < 100.0 || distToTarget > 22500.0) {
                FindNewTarget();
            }

            // MC: within 64 blocks and in sight — charge the fireball.
            if (m_dragon->DistanceToSqr(*m_attackTarget) < 4096.0) {
                if (m_dragon->GetSensing().HasLineOfSight(*m_attackTarget)) {
                    ++m_fireballCharge;

                    const glm::dvec3 aim = SafeNormalize(glm::dvec3(
                        m_attackTarget->position.x - m_dragon->position.x, 0.0,
                        m_attackTarget->position.z - m_dragon->position.z));
                    const float yRotRad = m_dragon->yRot * Mth::kDegToRad;
                    const glm::dvec3 dir = SafeNormalize(glm::dvec3(
                        std::sin(yRotRad), 0.0, -std::cos(yRotRad)));
                    const float dot = static_cast<float>(glm::dot(dir, aim));
                    float angleDegs = std::acos(Mth::Clamp(dot, -1.0f, 1.0f)) *
                                      Mth::kRadToDeg;
                    angleDegs += 0.5f;

                    if (m_fireballCharge >= kFireballChargeAmount &&
                        angleDegs >= 0.0f && angleDegs < 10.0f) {
                        // MC: the fireball leaves from just behind the head.
                        // Level event 1017 (the fireball roar) — no sounds.
                        const glm::dvec3 viewVector =
                            m_dragon->GetHeadLookVector();
                        const glm::dvec3 head = m_dragon->GetHeadPosition();
                        // MC head.getY(0.5) + 0.5 — half the head part's
                        // 1-block height, plus half a block.
                        const double startingX = head.x - viewVector.x * 1.0;
                        const double startingY = head.y + 0.5 + 0.5;
                        const double startingZ = head.z - viewVector.z * 1.0;
                        const glm::dvec3 direction(
                            m_attackTarget->position.x - startingX,
                            m_attackTarget->position.y +
                                static_cast<double>(
                                    m_attackTarget->GetBbHeight()) * 0.5 -
                                startingY,
                            m_attackTarget->position.z - startingZ);

                        auto fireball = std::make_unique<DragonFireball>(level);
                        fireball->SetOwnerAndDirection(*m_dragon,
                                                       SafeNormalize(direction));
                        fireball->position =
                            glm::dvec3(startingX, startingY, startingZ);
                        level->AddFreshEntity(std::move(fireball));

                        m_fireballCharge = 0;
                        if (m_hasPath) {
                            while (!m_currentPath.IsDone()) {
                                m_currentPath.Advance();
                            }
                        }
                        m_dragon->SetPhase(DragonPhase::HoldingPattern);
                    }
                } else if (m_fireballCharge > 0) {
                    --m_fireballCharge;
                }
            } else if (m_fireballCharge > 0) {
                --m_fireballCharge;
            }
        }

        void DragonStrafePlayerPhase::FindNewTarget() {
            // MC findNewTarget — the same ring walk as the holding pattern.
            EntityLevel* level = m_dragon->Level();
            if (!level) return;
            JavaRandom& rng = level->Random();

            if (!m_hasPath || m_currentPath.IsDone()) {
                const int currentNodeIndex = m_dragon->FindClosestNode();
                int targetNodeIndex = currentNodeIndex;
                if (rng.NextInt(8) == 0) {
                    m_holdingPatternClockwise = !m_holdingPatternClockwise;
                    targetNodeIndex = currentNodeIndex + 6;
                }
                if (m_holdingPatternClockwise) ++targetNodeIndex;
                else --targetNodeIndex;

                // No dragon fight → MC's inner-ring branch.
                targetNodeIndex -= 12;
                targetNodeIndex &= 7;
                targetNodeIndex += 12;

                m_hasPath = m_dragon->FindPath(currentNodeIndex, targetNodeIndex,
                                               nullptr, m_currentPath);
                if (m_hasPath) m_currentPath.Advance();
            }

            NavigateToNextPathNode();
        }

        void DragonStrafePlayerPhase::NavigateToNextPathNode() {
            if (m_hasPath && !m_currentPath.IsDone()) {
                const glm::ivec3 current = m_currentPath.NextNodePos();
                m_currentPath.Advance();

                JavaRandom& rng = m_dragon->Level()->Random();
                double yTarget;
                do {
                    yTarget = static_cast<double>(
                        static_cast<float>(current.y) + rng.NextFloat() * 20.0f);
                } while (yTarget < static_cast<double>(current.y));

                m_targetLocation = glm::dvec3(current.x, yTarget, current.z);
                m_hasTarget = true;
            }
        }

        // ── DragonTakeoffPhase ─────────────────────────────────────────────

        void DragonTakeoffPhase::DoServerTick() {
            // MC DragonTakeoffPhase.doServerTick: climb away from the podium,
            // then hand over to the holding pattern 10 blocks out.
            if (!m_firstTick && m_hasPath) {
                const glm::ivec3 egg = m_dragon->GetPodiumPos();
                const glm::dvec3 centre(egg.x + 0.5, egg.y + 0.5, egg.z + 0.5);
                if (glm::length(centre - m_dragon->position) >= 10.0) {
                    m_dragon->SetPhase(DragonPhase::HoldingPattern);
                }
            } else {
                m_firstTick = false;
                FindNewTarget();
            }
        }

        void DragonTakeoffPhase::FindNewTarget() {
            // MC findNewTarget: aim 40 blocks BEHIND the look direction at
            // node height. MC's y=105 is the End's node altitude; the fight
            // origin's height + 30 stands in (the node graph itself supplies
            // the real Y).
            const int currentNodeIndex = m_dragon->FindClosestNode();
            const glm::dvec3 look = m_dragon->GetHeadLookVector();
            const glm::ivec3& origin = m_dragon->GetFightOrigin();
            int targetNodeIndex = m_dragon->FindClosestNode(
                origin.x - look.x * 40.0, origin.y + 30.0,
                origin.z - look.z * 40.0);

            // No dragon fight → MC's inner-ring branch.
            targetNodeIndex -= 12;
            targetNodeIndex &= 7;
            targetNodeIndex += 12;

            m_hasPath = m_dragon->FindPath(currentNodeIndex, targetNodeIndex,
                                           nullptr, m_currentPath);
            NavigateToNextPathNode();
        }

        void DragonTakeoffPhase::NavigateToNextPathNode() {
            // MC's takeoff variant advances TWICE (skips the node under the
            // dragon).
            if (!m_hasPath) return;
            m_currentPath.Advance();
            if (m_currentPath.IsDone()) return;

            const glm::ivec3 current = m_currentPath.NextNodePos();
            m_currentPath.Advance();

            JavaRandom& rng = m_dragon->Level()->Random();
            double yTarget;
            do {
                yTarget = static_cast<double>(
                    static_cast<float>(current.y) + rng.NextFloat() * 20.0f);
            } while (yTarget < static_cast<double>(current.y));

            m_targetLocation = glm::dvec3(current.x, yTarget, current.z);
            m_hasTarget = true;
        }

        // ── DragonChargePlayerPhase ────────────────────────────────────────

        void DragonChargePlayerPhase::DoServerTick() {
            // MC DragonChargePlayerPhase.doServerTick.
            if (!m_hasTarget) {
                // MC warns "Aborting charge player as no target was set".
                m_dragon->SetPhase(DragonPhase::HoldingPattern);
                return;
            }
            if (m_timeSinceCharge > 0 && m_timeSinceCharge++ >= kChargeRecoveryTime) {
                m_dragon->SetPhase(DragonPhase::HoldingPattern);
                return;
            }
            const double distToTarget =
                glm::dot(m_targetLocation - m_dragon->position,
                         m_targetLocation - m_dragon->position);
            if (distToTarget < 100.0 || distToTarget > 22500.0 ||
                m_dragon->horizontalCollision || m_dragon->verticalCollision) {
                ++m_timeSinceCharge;
            }
        }

        // ── DragonDeathPhase ───────────────────────────────────────────────

        void DragonDeathPhase::DoServerTick() {
            // MC DragonDeathPhase.doServerTick: dive at the podium with
            // health pinned to 1; arrival (or a wall) is the actual death.
            // (doClientTick's explosion-emitter shower — no particle system.)
            ++m_time;
            if (!m_hasTarget) {
                const glm::ivec3 egg = m_dragon->GetPodiumPos();
                m_targetLocation = glm::dvec3(egg.x + 0.5, egg.y, egg.z + 0.5);
                m_hasTarget = true;
            }

            const double distToTarget =
                glm::dot(m_targetLocation - m_dragon->position,
                         m_targetLocation - m_dragon->position);
            if (!(distToTarget < 100.0) && !(distToTarget > 22500.0) &&
                !m_dragon->horizontalCollision && !m_dragon->verticalCollision) {
                m_dragon->SetHealth(1.0f);
            } else {
                m_dragon->SetHealth(0.0f);
                // MC's 200-tick death cinematic (float-up, rays, XP shower)
                // is skipped — the standard death path stands in.
                m_dragon->Die(MobDamageSource::Generic, nullptr);
            }
        }

    } // namespace

    // ── EnderDragon ────────────────────────────────────────────────────────

    void EnderDragon::CreateAttributes(AttributeMap& out) {
        // MC EnderDragon.createAttributes: MAX_HEALTH 200 on the mob base
        // (CAMERA_DISTANCE 16 has no attribute in this port).
        CreateMobAttributes(out);
        out.Register(Attribute::MaxHealth, 200.0);
    }

    EnderDragon::EnderDragon(EntityLevel* level)
        : Mob(EntityTypeId::EnderDragon, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // MC's constructor: the EIGHT sub-entity hitboxes — SKIPPED, one
        // whole-box entity (see the class comment) — noPhysics = true (the
        // mover has no ghost mode; CheckWalls carves instead), and the phase
        // manager starting HOVERING. No goals: MC registers none.
        SetPhase(DragonPhase::Hovering);
    }

    EnderDragon::~EnderDragon() = default;

    void EnderDragon::SetPhase(DragonPhase phase) {
        // MC EnderDragonPhaseManager.setPhase.
        if (m_currentPhase && m_currentPhase->GetPhase() == phase) return;
        if (m_currentPhase) m_currentPhase->End();
        m_currentPhase = &GetPhaseInstance(phase);
        // The server's phase id IS the anim byte (MC DATA_PHASE); the client
        // mirror arrives through SetAnimStateByte.
        m_currentPhase->Begin();
    }

    DragonPhase EnderDragon::GetPhase() const {
        if (m_level && m_level->IsClientSide()) {
            return static_cast<DragonPhase>(m_clientPhaseId);
        }
        return m_currentPhase ? m_currentPhase->GetPhase()
                              : DragonPhase::Hovering;
    }

    DragonPhaseInstance& EnderDragon::GetPhaseInstance(DragonPhase phase) {
        auto& slot = m_phases[static_cast<size_t>(phase)];
        if (!slot) {
            switch (phase) {
                case DragonPhase::HoldingPattern:
                    slot = std::make_unique<DragonHoldingPatternPhase>(this);
                    break;
                case DragonPhase::StrafePlayer:
                    slot = std::make_unique<DragonStrafePlayerPhase>(this);
                    break;
                case DragonPhase::Takeoff:
                    slot = std::make_unique<DragonTakeoffPhase>(this);
                    break;
                case DragonPhase::ChargingPlayer:
                    slot = std::make_unique<DragonChargePlayerPhase>(this);
                    break;
                case DragonPhase::Dying:
                    slot = std::make_unique<DragonDeathPhase>(this);
                    break;
                case DragonPhase::Hovering:
                    slot = std::make_unique<DragonHoverPhase>(this);
                    break;
                default:
                    // The perch cycle (LandingApproach/Landing/Sitting*) is
                    // unported; MC's getById falls back to HOLDING_PATTERN
                    // for an unknown id and so does this.
                    slot = std::make_unique<DragonHoldingPatternPhase>(this);
                    break;
            }
        }
        return *slot;
    }

    bool EnderDragon::IsPhaseSitting() const {
        // MC DragonPhaseInstance.isSitting — the hover and the three perch
        // phases.
        switch (GetPhase()) {
            case DragonPhase::Hovering:
            case DragonPhase::SittingFlaming:
            case DragonPhase::SittingScanning:
            case DragonPhase::SittingAttacking:
                return true;
            default:
                return false;
        }
    }

    uint8_t EnderDragon::GetAnimStateByte() const {
        return static_cast<uint8_t>(GetPhase());
    }

    void EnderDragon::SetAnimStateByte(uint8_t v) {
        m_clientPhaseId = v < static_cast<uint8_t>(DragonPhase::Count)
                              ? v
                              : static_cast<uint8_t>(DragonPhase::Hovering);
    }

    void EnderDragon::EnsureNodes() {
        // MC EnderDragon.findClosestNode's lazy node build — 12 outer nodes
        // at r=60, 8 middle at r=40 (+10 height), 4 inner at r=20, each at
        // the surface plus the adjustment. DEVIATION (class comment): MC's
        // coordinates are absolute around the End's 0,0 with a y floor of 73
        // (the island top); here they centre on the fight origin and floor at
        // its height, so the ring follows the dragon's spawn point.
        if (m_nodesBuilt) return;
        m_nodesBuilt = true;

        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        for (int i = 0; i < 24; ++i) {
            int yAdjustment = 5;
            int nodeX;
            int nodeZ;
            if (i < 12) {
                nodeX = static_cast<int>(std::floor(
                    60.0f * std::cos(2.0f * (-Mth::kPi + 0.2617994f *
                                             static_cast<float>(i)))));
                nodeZ = static_cast<int>(std::floor(
                    60.0f * std::sin(2.0f * (-Mth::kPi + 0.2617994f *
                                             static_cast<float>(i)))));
            } else if (i < 20) {
                const int multiplier = i - 12;
                nodeX = static_cast<int>(std::floor(
                    40.0f * std::cos(2.0f * (-Mth::kPi + (Mth::kPi / 8.0f) *
                                             static_cast<float>(multiplier)))));
                nodeZ = static_cast<int>(std::floor(
                    40.0f * std::sin(2.0f * (-Mth::kPi + (Mth::kPi / 8.0f) *
                                             static_cast<float>(multiplier)))));
                yAdjustment += 10;
            } else {
                const int multiplier = i - 20;
                nodeX = static_cast<int>(std::floor(
                    20.0f * std::cos(2.0f * (-Mth::kPi + (Mth::kPi / 4.0f) *
                                             static_cast<float>(multiplier)))));
                nodeZ = static_cast<int>(std::floor(
                    20.0f * std::sin(2.0f * (-Mth::kPi + (Mth::kPi / 4.0f) *
                                             static_cast<float>(multiplier)))));
            }

            nodeX += m_fightOrigin.x;
            nodeZ += m_fightOrigin.z;
            const int surfaceY =
                blocks ? MotionBlockingY(*blocks, nodeX, nodeZ) : m_fightOrigin.y;
            const int nodeY = std::max(m_fightOrigin.y, surfaceY + yAdjustment);
            m_nodes[i] = glm::ivec3(nodeX, nodeY, nodeZ);
        }
    }

    int EnderDragon::FindClosestNode() {
        return FindClosestNode(position.x, position.y, position.z);
    }

    int EnderDragon::FindClosestNode(double tX, double tY, double tZ) {
        // MC findClosestNode(x, y, z). With no dragon fight the search starts
        // at the inner nodes (index 12), exactly MC's crystals-gone branch.
        EnsureNodes();
        float closestDist = 10000.0f;
        int closestIndex = 0;
        const glm::ivec3 currentPos(static_cast<int>(std::floor(tX)),
                                    static_cast<int>(std::floor(tY)),
                                    static_cast<int>(std::floor(tZ)));
        for (int i = 12; i < 24; ++i) {
            const glm::ivec3 d = m_nodes[i] - currentPos;
            const float dist = static_cast<float>(d.x) * d.x +
                               static_cast<float>(d.y) * d.y +
                               static_cast<float>(d.z) * d.z;
            if (dist < closestDist) {
                closestDist = dist;
                closestIndex = i;
            }
        }
        return closestIndex;
    }

    bool EnderDragon::FindPath(int startIndex, int endIndex,
                               const glm::ivec3* finalTarget, FlightPath& out) {
        // MC EnderDragon.findPath — A* over the fixed 24-node graph with the
        // adjacency masks verbatim. (MC uses its BinaryHeap; a min-scan over
        // 24 nodes is the same algorithm with possibly different tie-breaks.)
        static constexpr int kAdjacency[24] = {
            6146,     8197,    8202,    16404,   32808,   32848,
            65696,    131392,  131712,  263424,  526848,  525313,
            1581057,  3166214, 2138120, 6373424, 4358208, 12910976,
            9044480,  9706496, 15216640, 13688832, 11763712, 8257536,
        };
        EnsureNodes();

        struct ANode {
            float g = 0.0f, h = 0.0f, f = 0.0f;
            int   cameFrom = -1;
            bool  closed = false;
            bool  inOpen = false;
        };
        ANode nodes[24];

        const auto dist = [&](int a, int b) {
            const glm::ivec3 d = m_nodes[a] - m_nodes[b];
            return std::sqrt(static_cast<float>(d.x) * d.x +
                             static_cast<float>(d.y) * d.y +
                             static_cast<float>(d.z) * d.z);
        };
        const auto reconstruct = [&](int from, int to) {
            out.nodes.clear();
            out.nextIndex = 0;
            std::vector<glm::ivec3> reversed;
            int node = to;
            while (node != -1) {
                reversed.push_back(m_nodes[node]);
                if (node == from) break;
                node = nodes[node].cameFrom;
            }
            out.nodes.assign(reversed.rbegin(), reversed.rend());
            // MC's finalNode is chained past the end.
            if (finalTarget) out.nodes.push_back(*finalTarget);
        };

        const int from = startIndex;
        const int to = endIndex;
        nodes[from].g = 0.0f;
        nodes[from].h = dist(from, to);
        nodes[from].f = nodes[from].h;
        nodes[from].inOpen = true;

        int closest = from;
        // MC minimumNodeIndex: 12 with no crystals (always, here).
        constexpr int kMinimumNodeIndex = 12;

        for (;;) {
            // Pop the open node with the smallest f.
            int openNode = -1;
            float best = 0.0f;
            for (int i = 0; i < 24; ++i) {
                if (nodes[i].inOpen && (openNode == -1 || nodes[i].f < best)) {
                    openNode = i;
                    best = nodes[i].f;
                }
            }
            if (openNode == -1) break;
            nodes[openNode].inOpen = false;

            if (openNode == to) {
                reconstruct(from, to);
                return true;
            }
            if (dist(openNode, to) < dist(closest, to)) {
                closest = openNode;
            }
            nodes[openNode].closed = true;

            for (int i = kMinimumNodeIndex; i < 24; ++i) {
                if ((kAdjacency[openNode] & (1 << i)) == 0) continue;
                if (nodes[i].closed) continue;
                const float tentative = nodes[openNode].g + dist(openNode, i);
                if (!nodes[i].inOpen || tentative < nodes[i].g) {
                    nodes[i].cameFrom = openNode;
                    nodes[i].g = tentative;
                    nodes[i].h = dist(i, to);
                    nodes[i].f = nodes[i].g + nodes[i].h;
                    nodes[i].inOpen = true;
                }
            }
        }

        if (closest == from) return false;
        // MC: fall back to a path toward the closest reachable node.
        reconstruct(from, closest);
        return true;
    }

    float EnderDragon::GetHeadYOffset() const {
        // MC getHeadYOffset.
        if (IsPhaseSitting()) return -1.0f;
        const DragonFlightHistory::Sample p0 = flightHistory.Get(5);
        const DragonFlightHistory::Sample p1 = flightHistory.Get(0);
        return static_cast<float>(p0.y - p1.y);
    }

    glm::dvec3 EnderDragon::GetHeadPosition() const {
        // The head part per MC aiStep's tickPart(head, ...) layout — 6.5
        // blocks ahead along the yRotA-corrected facing, tilted by the
        // flight history (multipart entities skipped; the MATH is kept).
        const float tilt =
            static_cast<float>(flightHistory.Get(5).y - flightHistory.Get(10).y) *
            10.0f * Mth::kDegToRad;
        const float ccTilt = std::cos(tilt);
        const float ssTilt = std::sin(tilt);
        const float rot = yRot * Mth::kDegToRad - yRotA * 0.01f;
        const float ss2 = std::sin(rot);
        const float cc2 = std::cos(rot);
        const float yOffset = GetHeadYOffset();
        return position + glm::dvec3(ss2 * 6.5f * ccTilt,
                                     yOffset + ssTilt * 6.5f,
                                     -cc2 * 6.5f * ccTilt);
    }

    glm::dvec3 EnderDragon::GetHeadLookVector() const {
        // MC getHeadLookVector(1.0F).
        const DragonPhase phase = GetPhase();
        if (phase == DragonPhase::Landing || phase == DragonPhase::Takeoff) {
            const glm::ivec3 egg = GetPodiumPos();
            const double dx = egg.x + 0.5 - position.x;
            const double dy = egg.y + 0.5 - position.y;
            const double dz = egg.z + 0.5 - position.z;
            const float dist = std::max(
                static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz)) / 4.0f,
                1.0f);
            const float yOffset = 6.0f / dist;
            return glm::dvec3(Mth::ViewVector(-yOffset * 1.5f * 5.0f, yRot));
        }
        if (IsPhaseSitting()) {
            return glm::dvec3(Mth::ViewVector(-45.0f, yRot));
        }
        return glm::dvec3(Mth::ViewVector(xRot, yRot));
    }

    glm::ivec3 EnderDragon::GetPodiumPos() const {
        // MC EndPodiumFeature.getLocation(fightOrigin) + heightmapPos — the
        // fight-origin column's surface stands in for the End podium.
        const IBlockAccess* blocks = m_level ? m_level->Blocks() : nullptr;
        const int y = blocks
            ? MotionBlockingY(*blocks, m_fightOrigin.x, m_fightOrigin.z)
            : m_fightOrigin.y;
        return glm::ivec3(m_fightOrigin.x, y, m_fightOrigin.z);
    }

    double EnderDragon::DistanceToPodiumSqr() const {
        const glm::ivec3 egg = GetPodiumPos();
        const double dx = egg.x + 0.5 - position.x;
        const double dy = egg.y + 0.5 - position.y;
        const double dz = egg.z + 0.5 - position.z;
        return dx * dx + dy * dy + dz * dz;
    }

    void EnderDragon::AiStep() {
        // MC EnderDragon.aiStep — REPLACES Mob::AiStep wholesale, exactly as
        // MC's override replaces Mob's (no goals, no travel; the flight below
        // IS the locomotion). processFlappingMovement (the flap sound) and
        // the client growl timer are sound-system work, skipped.
        const bool clientSide = m_level && m_level->IsClientSide();

        oFlapTime = flapTime;
        if (IsDeadOrDying()) {
            // MC: random explosion particles around the corpse — no particle
            // system. (tickDeath's +0.1/tick float and the 200-tick timer are
            // the skipped death cinematic; the standard 20-tick death runs.)
            return;
        }

        // MC checkCrystals — crystal healing SKIPPED: no EndCrystal entity.

        // The flap clock. The server measures real velocity; the client copy
        // is position-synced, so it measures its own per-tick displacement.
        glm::dvec3 movement = velocity;
        if (clientSide) {
            movement = m_prevPosForFlapValid ? position - m_prevPosForFlap
                                             : glm::dvec3(0.0);
        }
        float flapSpeed =
            0.2f / (static_cast<float>(
                        std::sqrt(movement.x * movement.x +
                                  movement.z * movement.z)) *
                        10.0f +
                    1.0f);
        flapSpeed *= static_cast<float>(std::pow(2.0, movement.y));
        if (IsPhaseSitting()) {
            flapTime += 0.1f;
        } else if (inWall) {
            flapTime += flapSpeed * 0.5f;
        } else {
            flapTime += flapSpeed;
        }
        m_prevPosForFlap = position;
        m_prevPosForFlapValid = true;

        yRot = Mth::WrapDegrees(yRot);
        if (IsNoAi()) {
            flapTime = 0.5f;
            return;
        }

        flightHistory.Record(position.y, yRot);

        if (!clientSide) {
            DragonPhaseInstance* currentPhase = m_currentPhase;
            if (!currentPhase) {
                SetPhase(DragonPhase::Hovering);
                currentPhase = m_currentPhase;
            }
            currentPhase->DoServerTick();
            if (m_currentPhase != currentPhase) {
                // MC: a phase switch mid-tick gets its own server tick.
                currentPhase = m_currentPhase;
                currentPhase->DoServerTick();
            }

            glm::dvec3 targetLocation;
            if (currentPhase->GetFlyTargetLocation(targetLocation)) {
                // ── The flight physics, transcribed number for number ─────
                const double xdd = targetLocation.x - position.x;
                double ydd = targetLocation.y - position.y;
                const double zdd = targetLocation.z - position.z;
                const double distToTarget = xdd * xdd + ydd * ydd + zdd * zdd;
                const float maxClimb = currentPhase->GetFlySpeed();
                const double horizontalDist = std::sqrt(xdd * xdd + zdd * zdd);
                if (horizontalDist > 0.0) {
                    ydd = Mth::Clamp(ydd / horizontalDist,
                                     static_cast<double>(-maxClimb),
                                     static_cast<double>(maxClimb));
                }

                velocity.y += ydd * 0.01;
                yRot = Mth::WrapDegrees(yRot);
                const glm::dvec3 aim =
                    SafeNormalize(targetLocation - position);
                const glm::dvec3 dir = SafeNormalize(glm::dvec3(
                    std::sin(yRot * Mth::kDegToRad), velocity.y,
                    -std::cos(yRot * Mth::kDegToRad)));
                const float dot = std::max(
                    (static_cast<float>(glm::dot(dir, aim)) + 0.5f) / 1.5f,
                    0.0f);

                if (std::abs(xdd) > 1.0e-5 || std::abs(zdd) > 1.0e-5) {
                    const float yRotD = Mth::Clamp(
                        Mth::WrapDegrees(
                            180.0f -
                            static_cast<float>(std::atan2(xdd, zdd)) *
                                Mth::kRadToDeg -
                            yRot),
                        -50.0f, 50.0f);
                    yRotA *= 0.8f;
                    yRotA += yRotD * currentPhase->GetTurnSpeed();
                    yRot += yRotA * 0.1f;
                }

                const float span =
                    static_cast<float>(2.0 / (distToTarget + 1.0));
                MoveRelative(0.06f * (dot * span + (1.0f - span)),
                             glm::dvec3(0.0, 0.0, -1.0));
                if (inWall) {
                    Move(velocity * 0.8);
                } else {
                    Move(velocity);
                }

                const glm::dvec3 actual = SafeNormalize(velocity);
                const double slide =
                    0.8 + 0.15 * (glm::dot(actual, dir) + 1.0) / 2.0;
                velocity.x *= slide;
                velocity.y *= 0.91;
                velocity.z *= slide;
            }
        }
        // else: MC's client half is interpolation (ClientMobManager's job)
        // and doClientTick (death particles only — skipped).

        // MC applyEffectsFromBlocks — no block contact effects modelled.

        yBodyRot = yRot;

        // ── The part sweeps (MC's wing knockback + head/neck bite) ─────────
        // The eight sub-entities are skipped; their boxes are rebuilt from
        // MC's tickPart layout so the sweeps land where MC's parts sit.
        if (!clientSide && hurtTime == 0) {
            const float rot1 = yRot * Mth::kDegToRad;
            const float ss1 = std::sin(rot1);
            const float cc1 = std::cos(rot1);

            // Wings: 4x2 parts at (±cc1*4.5, +2, ±ss1*4.5), swept inflated
            // (4,2,4) and dropped 2 (MC's .inflate(4,2,4).move(0,-2,0)).
            const auto wingSweepBox = [&](double side) {
                AABB box = PartBox(position + glm::dvec3(cc1 * 4.5 * side, 2.0,
                                                         ss1 * 4.5 * side),
                                   4.0f, 2.0f);
                box.min -= glm::vec3(4.0f, 2.0f, 4.0f);
                box.max += glm::vec3(4.0f, 2.0f, 4.0f);
                box.min.y -= 2.0f;
                box.max.y -= 2.0f;
                return box;
            };
            KnockBackNearby(wingSweepBox(1.0));
            KnockBackNearby(wingSweepBox(-1.0));

            // Head (1x1) and neck (3x3), each inflated 1.
            const glm::dvec3 head = GetHeadPosition();
            // The neck sits at 5.5 of the head's 6.5 along the same ray.
            const glm::dvec3 neck = position + (head - position) * (5.5 / 6.5);
            const auto inflate1 = [](AABB box) {
                box.min -= glm::vec3(1.0f);
                box.max += glm::vec3(1.0f);
                return box;
            };
            HurtNearby(inflate1(PartBox(head, 1.0f, 1.0f)));
            HurtNearby(inflate1(PartBox(neck, 3.0f, 3.0f)));
        }

        if (!clientSide) {
            // MC: checkWalls over the HEAD, NECK and BODY part boxes — NOT
            // the 16-wide whole hitbox, which would carve a canyon. The
            // boxes are rebuilt from the tickPart layout (multipart skipped):
            // body 5x3 half a block behind centre, neck 3x3 at 5.5, head 1x1
            // at 6.5.
            const float rot1 = yRot * Mth::kDegToRad;
            const float ss1 = std::sin(rot1);
            const float cc1 = std::cos(rot1);
            const glm::dvec3 head = GetHeadPosition();
            const glm::dvec3 neck = position + (head - position) * (5.5 / 6.5);
            const AABB bodyBox = PartBox(
                position + glm::dvec3(ss1 * 0.5, 0.0, -cc1 * 0.5), 5.0f, 3.0f);
            const bool a = CheckWalls(PartBox(head, 1.0f, 1.0f));
            const bool b = CheckWalls(PartBox(neck, 3.0f, 3.0f));
            const bool c = CheckWalls(bodyBox);
            inWall = a || b || c;
            // MC dragonFight.updateDragon — no End fight layer.
        }
    }

    void EnderDragon::KnockBackNearby(const AABB& box) {
        // MC EnderDragon.knockBack: shove everything living away from the
        // body's centre, and (while not sitting) deal the 5.0 wing hit.
        if (!m_level) return;
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p && p->GetAABB().Intersects(box)) nearby.push_back(p);
        }

        // MC uses the BODY part's centre; the whole box's centre is the same
        // point with the parts skipped.
        const double xm = position.x;
        const double zm = position.z;

        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            // MC EntitySelector.NO_CREATIVE_OR_SPECTATOR.
            if (living->IsCreative() || living->IsSpectator()) continue;

            const double xd = living->position.x - xm;
            const double zd = living->position.z - zm;
            const double dd = std::max(xd * xd + zd * zd, 0.1);
            living->AddDeltaMovement(
                glm::dvec3(xd / dd * 4.0, 0.2, zd / dd * 4.0));
            if (!IsPhaseSitting() &&
                living->GetLastHurtByMobTimestamp() < living->tickCount - 2) {
                living->Hurt(MobDamageSource::MobAttack, 5.0f, this);
            }
        }
    }

    void EnderDragon::HurtNearby(const AABB& box) {
        // MC EnderDragon.hurt(List<Entity>): the 10.0 head/neck bite.
        if (!m_level) return;
        std::vector<Entity*> nearby;
        m_level->GetEntitiesInBox(box, this, nearby);
        std::vector<LivingEntity*> players;
        m_level->GetPlayers(players);
        for (LivingEntity* p : players) {
            if (p && p->GetAABB().Intersects(box)) nearby.push_back(p);
        }
        for (Entity* e : nearby) {
            auto* living = dynamic_cast<LivingEntity*>(e);
            if (!living || !living->IsAlive()) continue;
            if (living->IsCreative() || living->IsSpectator()) continue;
            living->Hurt(MobDamageSource::MobAttack, 10.0f, this);
        }
    }

    bool EnderDragon::CheckWalls(const AABB& box) {
        // MC checkWalls: smash every non-immune, non-transparent block the
        // box overlaps (mobGriefing); an immune block registers as a wall.
        // Level event 2008 (the block-break puff) — no particle system.
        if (!m_level || !m_level->Blocks()) return false;
        const IBlockAccess& blocks = *m_level->Blocks();

        const int x0 = static_cast<int>(std::floor(box.min.x));
        const int y0 = static_cast<int>(std::floor(box.min.y));
        const int z0 = static_cast<int>(std::floor(box.min.z));
        const int x1 = static_cast<int>(std::floor(box.max.x));
        const int y1 = static_cast<int>(std::floor(box.max.y));
        const int z1 = static_cast<int>(std::floor(box.max.z));
        bool hitWall = false;

        const bool griefing = m_level->MobGriefing();
        for (int x = x0; x <= x1; ++x) {
            for (int y = y0; y <= y1; ++y) {
                for (int z = z0; z <= z1; ++z) {
                    const BlockID id = blocks.GetBlock(x, y, z);
                    if (IsDragonTransparentBlock(id)) continue;
                    if (griefing && !IsDragonImmuneBlock(id)) {
                        // MC removeBlock(pos, false) — no drops.
                        m_level->DestroyBlock(glm::ivec3(x, y, z), false);
                    } else {
                        hitWall = true;
                    }
                }
            }
        }
        return hitWall;
    }

    bool EnderDragon::Hurt(MobDamageSource source, float amount,
                           Entity* attacker) {
        // MC EnderDragon.hurt(part, source, damage) with part == body always
        // (multipart hitboxes skipped) — so every hit takes the non-head
        // reduction, which is also what MC's plain hurtServer routes to.
        if (!m_level || m_level->IsClientSide()) return false;
        if (GetPhase() == DragonPhase::Dying) return false;

        if (m_currentPhase) amount = m_currentPhase->OnHurt(amount);
        amount = amount / 4.0f + std::min(amount, 1.0f);
        if (amount < 0.01f) return false;

        // MC: only players (and ALWAYS_HURTS_ENDER_DRAGONS — end-crystal
        // explosions, which do not exist here) actually damage the dragon.
        if (attacker != nullptr && attacker->IsPlayer()) {
            const float healthBefore = GetHealth();
            // MC reallyHurt == super.hurtServer; the DYING pin on a lethal
            // hit lives in the Die override below.
            Mob::Hurt(source, amount, attacker);

            if (IsPhaseSitting()) {
                m_sittingDamageReceived += healthBefore - GetHealth();
                if (m_sittingDamageReceived > 0.25f * GetMaxHealth()) {
                    m_sittingDamageReceived = 0.0f;
                    SetPhase(DragonPhase::Takeoff);
                }
            }
        }
        return true;
    }

    void EnderDragon::Die(MobDamageSource source, Entity* attacker) {
        // MC EnderDragon.hurt: a lethal hit while FLYING pins health at 1 and
        // enters the DYING dive; only the dive's arrival (or a lethal hit
        // while perched) actually kills. The 200-tick death cinematic is the
        // skipped render half — the standard 20-tick death stands in.
        if (m_level && !m_level->IsClientSide() &&
            GetPhase() != DragonPhase::Dying && !IsPhaseSitting()) {
            SetHealth(1.0f);
            SetPhase(DragonPhase::Dying);
            return;
        }
        Mob::Die(source, attacker);
    }

    void EnderDragon::Knockback(double power, double dx, double dz) {
        // MC EnderDragon.knockback: suppressed while sitting only (the
        // flight physics overwrite it in the air anyway, as in MC).
        if (!IsPhaseSitting()) {
            Mob::Knockback(power, dx, dz);
        }
    }

    std::shared_ptr<SpawnGroupData>
    EnderDragon::FinalizeSpawn(SpawnReason reason,
                               std::shared_ptr<SpawnGroupData> groupData) {
        groupData = Mob::FinalizeSpawn(reason, std::move(groupData));
        if (!m_fightOriginSet) {
            m_fightOrigin = BlockPosition();
            m_fightOriginSet = true;
        }
        // DEVIATION, documented: MC's EndDragonFight spawns its dragon into
        // the phase cycle while a bare /summon hovers forever; here the
        // summoned dragon IS the fight dragon, so it starts the holding
        // pattern around its spawn point (the "podium" anchor).
        SetPhase(DragonPhase::HoldingPattern);
        return groupData;
    }

    void EnderDragon::ClearReferenceTo(const Entity* entity) {
        Mob::ClearReferenceTo(entity);
        for (auto& phase : m_phases) {
            if (phase) phase->ClearReferenceTo(entity);
        }
    }

} // namespace Game
