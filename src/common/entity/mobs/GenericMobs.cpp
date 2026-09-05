// File: src/common/entity/mobs/GenericMobs.cpp
#include "common/entity/mobs/GenericMobs.hpp"

#include "common/entity/EntityLevel.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/mobs/Fish.hpp"

#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/navigation/AmphibiousPathNavigation.hpp"
#include "common/entity/ai/navigation/FlyingPathNavigation.hpp"
#include "common/entity/ai/navigation/WaterBoundPathNavigation.hpp"
#include "common/world/crafting/RecipeManager.hpp"

#include <algorithm>
#include <vector>

namespace Game {

    namespace {

        // Apply a def's attribute overrides on top of whatever the base
        // registered. Order matters: the base runs first (it establishes which
        // attributes exist at all), then MC's per-mob createAttributes values
        // replace the defaults.
        void ApplyDef(AttributeMap& attrs, const MobDef& def) {
            for (int i = 0; i < def.attrCount; ++i) {
                const MobAttrOverride& o = kMobAttrs[def.firstAttr + i];
                attrs.Register(o.attribute, o.value);
            }
        }

        // MC states isFood as an item tag; the generator flattens it to slugs
        // because a food may be a block item (cactus, bamboo, seagrass, every
        // flower a bee likes) with no ItemID constant. Resolve once — the item
        // registry is not populated when the def table is.
        const std::vector<ItemID>& FoodFor(const MobDef& def) {
            static std::vector<std::vector<ItemID>> cache(kMobDefCount);
            static std::vector<bool> done(kMobDefCount, false);
            const size_t i = static_cast<size_t>(&def - kMobDefs);
            if (i >= cache.size()) {
                static const std::vector<ItemID> kNone;
                return kNone;
            }
            if (!done[i]) {
                done[i] = true;
                for (int k = 0; k < def.foodCount; ++k) {
                    const ItemID id = RecipeManager::ItemFromSlug(
                        kMobFoodSlugs[def.firstFood + k]);
                    if (id != Items::Air) cache[i].push_back(id);
                }
            }
            return cache[i];
        }

        const MobDef& DefFor(EntityTypeId type) {
            const MobDef* d = FindMobDef(type);
            // MakeGenericMob is the only caller and it checks first, so a null
            // here is a programming error rather than a runtime condition.
            static const MobDef kEmpty{ EntityTypeId::Count, MobBase::PathfinderMob,
                                        "", 0, 0, 0, 0,
                                        1.0, 2.0, 1.0, 8.0f, 1.0f, true,
                                        4.0f, 1.0f, 0.4f, 3.0f };
            return d ? *d : kEmpty;
        }

        // MC picks the wander goal per LOCOMOTION: swimmers get
        // RandomSwimmingGoal, fliers WaterAvoidingRandomFlyingGoal, and
        // walkers one of the two stroll variants (8 mobs use the plain one,
        // which will walk into water).
        void AddStroll(PathfinderMob& mob, GoalSelector& goals,
                       const MobDef& def, int priority) {
            if (def.swimStrollSpeed > 0.0) {
                goals.AddGoal(priority, std::make_unique<RandomSwimmingGoal>(
                                            &mob, def.swimStrollSpeed,
                                            def.swimStrollInterval));
                return;
            }
            if (def.nav == MobNav::Flying) {
                // flyStrollSpeed is 0 for mobs whose wander goal is a nested
                // subclass in its own file (parrot, bee); MC passes 1.0 there.
                const double speed = def.flyStrollSpeed > 0.0 ? def.flyStrollSpeed : 1.0;
                goals.AddGoal(priority, std::make_unique<WaterAvoidingRandomFlyingGoal>(
                                            &mob, speed));
                return;
            }
            if (def.strollSpeed <= 0.0) return;
            if (def.strollAvoidsWater) {
                goals.AddGoal(priority, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                            &mob, def.strollSpeed));
            } else {
                goals.AddGoal(priority, std::make_unique<RandomStrollGoal>(
                                            &mob, def.strollSpeed));
            }
        }

        // MC declares locomotion in createNavigation + the control
        // constructors; the def carries both, and this applies them. Called
        // after the base constructor built the ground defaults.
        void ApplyLocomotion(Mob& mob, EntityLevel* level, const MobDef& def) {
            switch (def.nav) {
                case MobNav::Flying:
                    mob.SetNavigation(std::make_unique<FlyingPathNavigation>(&mob, level));
                    break;
                case MobNav::Water:
                    mob.SetNavigation(std::make_unique<WaterBoundPathNavigation>(&mob, level));
                    break;
                case MobNav::Amphibious:
                    mob.SetNavigation(std::make_unique<AmphibiousPathNavigation>(&mob, level));
                    break;
                case MobNav::WallClimber:
                    mob.SetNavigation(std::make_unique<WallClimberNavigation>(&mob, level));
                    break;
                case MobNav::Ground:
                    break;
            }
            if (def.flyCtrl) {
                mob.SetMoveControl(std::make_unique<FlyingMoveControl>(
                    &mob, def.flyMaxTurn, def.flyHover));
            }
            if (def.swimCtrl) {
                mob.SetMoveControl(std::make_unique<SmoothSwimmingMoveControl>(
                    &mob, def.swimMaxTurnX, def.swimMaxTurnY, def.swimInWater,
                    def.swimOutsideWater, def.swimGravity));
                // The REAL control owns the outside-water modifier now; the
                // SetSpeed-side stand-in must not apply it a second time.
                mob.SetLandSpeedFactor(1.0f);
            }
            if (def.fishCtrl) {
                mob.SetMoveControl(std::make_unique<FishMoveControl>(&mob));
            }
            if (def.swimLookMaxYRot > 0) {
                mob.SetLookControl(std::make_unique<SmoothSwimmingLookControl>(
                    &mob, def.swimLookMaxYRot));
            }
        }

    } // namespace

    // ── Mob ────────────────────────────────────────────────────────────────

    namespace {

        // MC's brain mobs look at the player on a uniform timer instead of
        // LookAtPlayerGoal's 2%-per-poll roll — see the goal's UseInterval.
        // Eight of the generated mobs are marked that way in their def.
        // MC's NearestAttackableTargetGoal / AvoidEntityGoal, from the type
        // lists the generator read out of registerGoals. Registering them here
        // rather than per mob is what gives 39 mobs the targets MC gives them —
        // a zombie that hunts villagers and iron golems, a creeper that runs
        // from ocelots, a wandering trader that flees every illager.
        void AddTargetGoals(Mob* mob, GoalSelector& targets, const MobDef& def) {
            if (def.targetsPlayers) {
                targets.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(mob, true));
            }
            if (def.targetTypeCount > 0) {
                targets.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                    mob, kMobTargetTypes + def.firstTargetType, def.targetTypeCount, true));
            }
        }

        void AddAvoidGoal(PathfinderMob* mob, GoalSelector& goals, const MobDef& def) {
            // MC's speeds vary per mob (1.0/1.2 for a creeper fleeing an
            // ocelot, 0.8/1.33 for a wandering trader). The generator does not
            // read them yet, so this uses MC's most common pair and says so.
            if (def.avoidsPlayers) {
                goals.AddGoal(3, std::make_unique<AvoidEntityGoal>(mob, 8.0f, 1.0, 1.2));
            }
            if (def.avoidTypeCount > 0) {
                goals.AddGoal(3, std::make_unique<AvoidEntityGoal>(
                    mob, kMobAvoidTypes + def.firstAvoidType, def.avoidTypeCount,
                    6.0f, 1.0, 1.2));
            }
        }

        std::unique_ptr<LookAtPlayerGoal> MakeLookAtPlayer(Mob* mob, const MobDef& def) {
            auto goal = std::make_unique<LookAtPlayerGoal>(mob, def.lookDistance);
            if (def.lookIntervalMin > 0) {
                goal->UseInterval(def.lookIntervalMin, def.lookIntervalMax);
            }
            return goal;
        }

    } // namespace

    GenericMob::GenericMob(EntityTypeId type, EntityLevel* level)
        : Mob(type, level) {
        CreateMobAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
        ApplyLocomotion(*this, level, DefFor(type));
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void GenericMob::RegisterGoals() {
        // No stroll: see the header. Float keeps it from drowning in place and
        // the two look goals give it MC's idle head movement, which is all a
        // MC Mob with no navigation does on land anyway.
        const MobDef& def = DefFor(GetType());
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, MakeLookAtPlayer(this, def));
        m_goalSelector.AddGoal(2, std::make_unique<RandomLookAroundGoal>(this));
    }

    // ── PathfinderMob ──────────────────────────────────────────────────────

    GenericPathfinderMob::GenericPathfinderMob(EntityTypeId type, EntityLevel* level)
        : PathfinderMob(type, level) {
        CreateMobAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
        ApplyLocomotion(*this, level, DefFor(type));
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void GenericPathfinderMob::RegisterGoals() {
        const MobDef& def = DefFor(GetType());
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        AddStroll(*this, m_goalSelector, def, 5);
        m_goalSelector.AddGoal(6, MakeLookAtPlayer(this, def));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        AddTargetGoals(this, m_targetSelector, def);
        AddAvoidGoal(this, m_goalSelector, def);
    }

    // ── AgeableMob (villager) ──────────────────────────────────────────────

    GenericAgeableMob::GenericAgeableMob(EntityTypeId type, EntityLevel* level)
        : AgeableMob(type, level) {
        CreateMobAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
        ApplyLocomotion(*this, level, DefFor(type));
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    bool GenericAgeableMob::IsFlyingAnimal() const {
        return DefFor(GetType()).flyingAnimal;
    }

    void GenericAgeableMob::RegisterGoals() {
        // The same set as GenericPathfinderMob — MC PathfinderMob's default.
        const MobDef& def = DefFor(GetType());
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        AddStroll(*this, m_goalSelector, def, 5);
        m_goalSelector.AddGoal(6, MakeLookAtPlayer(this, def));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        AddTargetGoals(this, m_targetSelector, def);
        AddAvoidGoal(this, m_goalSelector, def);
    }

    // ── Monster ────────────────────────────────────────────────────────────

    GenericMonster::GenericMonster(EntityTypeId type, EntityLevel* level)
        : Monster(type, level) {
        CreateMonsterAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
        ApplyLocomotion(*this, level, DefFor(type));
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void GenericMonster::RegisterGoals() {
        // The shape every melee monster in MC shares. Priorities are MC's for
        // Zombie, which is the archetype the others vary from.
        const MobDef& def = DefFor(GetType());
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<MeleeAttackGoal>(this, def.meleeSpeed, false));
        AddStroll(*this, m_goalSelector, def, 7);
        m_goalSelector.AddGoal(8, MakeLookAtPlayer(this, def));
        m_goalSelector.AddGoal(8, std::make_unique<RandomLookAroundGoal>(this));

        m_targetSelector.AddGoal(1, std::make_unique<HurtByTargetGoal>(this));
        AddTargetGoals(this, m_targetSelector, def);
        AddAvoidGoal(this, m_goalSelector, def);
    }

    // ── Endermite ──────────────────────────────────────────────────────────

    void Endermite::AiStep() {
        GenericMonster::AiStep();
        // MC Endermite.aiStep: the client half scatters PORTAL particles (no
        // particle system yet); the server half ages the mite out at 2400
        // ticks, paused while persistence is required (a name tag).
        if (!m_level || m_level->IsClientSide()) return;
        if (!IsPersistenceRequired()) ++m_life;
        if (m_life >= kMaxLife) Discard();
    }

    // ── Animal ─────────────────────────────────────────────────────────────

    GenericAnimal::GenericAnimal(EntityTypeId type, EntityLevel* level)
        : Animal(type, level) {
        CreateAnimalAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
        ApplyLocomotion(*this, level, DefFor(type));
        m_health = GetMaxHealth();
        RegisterGoals();
    }

    void GenericAnimal::RegisterGoals() {
        // MC's shared animal set. The food comes from MC's own
        // ItemTags.<MOB>_FOOD, flattened by the generator, so TemptGoal and
        // BreedGoal can finally be registered — 29 of the generated animals
        // have one.
        const MobDef& def = DefFor(GetType());
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, def.panicSpeed));
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        // MC's own goals now that the food is known: BreedGoal at 2 and
        // TemptGoal at 3, the priorities AbstractCow.registerGoals uses. Both
        // are skipped when isFood accepts nothing, which is MC's behaviour for
        // an animal that cannot be fed.
        if (!FoodFor(def).empty()) {
            m_goalSelector.AddGoal(2, std::make_unique<BreedGoal>(this, 1.0));
            m_goalSelector.AddGoal(3, std::make_unique<TemptGoal>(this, 1.25, false));
        }
        m_goalSelector.AddGoal(4, std::make_unique<FollowParentGoal>(this, 1.25));
        AddStroll(*this, m_goalSelector, def, 5);
        m_goalSelector.AddGoal(6, MakeLookAtPlayer(this, def));
        m_goalSelector.AddGoal(7, std::make_unique<RandomLookAroundGoal>(this));
        AddTargetGoals(this, m_targetSelector, def);
        AddAvoidGoal(this, m_goalSelector, def);
    }

    bool GenericAnimal::IsFood(uint32_t itemId) const {
        const std::vector<ItemID>& food = FoodFor(DefFor(GetType()));
        return std::find(food.begin(), food.end(), static_cast<ItemID>(itemId))
               != food.end();
    }

    std::unique_ptr<Animal> GenericAnimal::CreateBaby() {
        // Through the factory, so a PROMOTED type breeds a promoted baby — a
        // camel foal or baby sniffer built as a plain GenericAnimal would have
        // no brain and stand inert forever. AgeableMob's own breeding path
        // sets the baby age, exactly as the hand-written Cow/Pig/Sheep do.
        std::unique_ptr<Mob> baby = MakeGenericMob(GetType(), m_level);
        if (dynamic_cast<Animal*>(baby.get())) {
            return std::unique_ptr<Animal>(static_cast<Animal*>(baby.release()));
        }
        return std::make_unique<GenericAnimal>(GetType(), m_level);
    }


    bool GenericMob::IsFlyingAnimal() const { return DefFor(GetType()).flyingAnimal; }
    bool GenericPathfinderMob::IsFlyingAnimal() const { return DefFor(GetType()).flyingAnimal; }
    bool GenericMonster::IsFlyingAnimal() const { return DefFor(GetType()).flyingAnimal; }
    bool GenericAnimal::IsFlyingAnimal() const { return DefFor(GetType()).flyingAnimal; }

    // ── Factory ────────────────────────────────────────────────────────────

    std::unique_ptr<Mob> MakeGenericMob(EntityTypeId type, EntityLevel* level) {
        const MobDef* def = FindMobDef(type);
        if (!def) return nullptr;

        // Mobs promoted out of the generic path because their animation state
        // machine is real behaviour worth porting. They still take the def's
        // attributes and goal set — the subclass only adds what MC adds.
        //
        // This switch is here rather than beside the eight hand-written mobs'
        // switch because BOTH factories (IntegratedServer and ClientMobManager)
        // fall through to this function, and a type the server builds but the
        // client does not is a mob that ticks and never draws.
        switch (type) {
            case EntityTypeId::Endermite: return std::make_unique<Endermite>(level);
            case EntityTypeId::Frog:      return std::make_unique<Frog>(level);
            case EntityTypeId::Axolotl:   return std::make_unique<Axolotl>(level);
            case EntityTypeId::Camel:     return std::make_unique<Camel>(level);
            // MC's CamelHusk extends Camel — same brain, same pose machinery.
            case EntityTypeId::CamelHusk:
                return std::make_unique<Camel>(level, EntityTypeId::CamelHusk);
            case EntityTypeId::Bat:       return std::make_unique<Bat>(level);
            case EntityTypeId::Armadillo: return std::make_unique<Armadillo>(level);
            case EntityTypeId::Tadpole:   return std::make_unique<Tadpole>(level);
            case EntityTypeId::Goat:      return std::make_unique<Goat>(level);
            case EntityTypeId::Hoglin:    return std::make_unique<Hoglin>(level);
            // Bee keeps the def's goal set (it extends the generic base) and
            // adds MC's animation machinery — the hover-roll.
            case EntityTypeId::Zoglin:    return std::make_unique<Zoglin>(level);
            case EntityTypeId::Piglin:    return std::make_unique<Piglin>(level);
            case EntityTypeId::PiglinBrute:
                return std::make_unique<PiglinBrute>(level);
            case EntityTypeId::Allay:     return std::make_unique<Allay>(level);
            case EntityTypeId::Nautilus:  return std::make_unique<Nautilus>(level);
            case EntityTypeId::ZombieNautilus:
                return std::make_unique<ZombieNautilus>(level);
            case EntityTypeId::Bee:       return std::make_unique<Bee>(level);
            // Wolf keeps the def's goal set and adds the persistent-anger
            // system (NeutralMob), the taming layer (TamableAnimal) and
            // MC's leap + melee goals — see Animals.hpp.
            case EntityTypeId::Wolf:      return std::make_unique<Wolf>(level);
            // Mooshroom keeps the def's goal set and adds the shears →
            // cow conversion (MushroomCow.mobInteract) — see Animals.hpp.
            case EntityTypeId::Mooshroom: return std::make_unique<Mooshroom>(level);
            case EntityTypeId::Warden:    return std::make_unique<Warden>(level);
            case EntityTypeId::Creaking:  return std::make_unique<Creaking>(level);
            case EntityTypeId::Breeze:    return std::make_unique<Breeze>(level);
            case EntityTypeId::Sniffer:   return std::make_unique<Sniffer>(level);
            case EntityTypeId::CopperGolem:
                return std::make_unique<CopperGolem>(level);
            // MC Villager extends AbstractVillager extends AgeableMob: the
            // age is what a baby villager IS. Goal set unchanged.
            case EntityTypeId::Villager:
                return std::make_unique<GenericAgeableMob>(type, level);
            // Silverfish stays generic: both of its bespoke goals need
            // infested blocks, which this engine does not have —
            // SilverfishWakeUpFriendsGoal bursts hidden silverfish OUT of
            // infested stone around a hurt one, and SilverfishMergeWithStone
            // burrows INTO a stone block, converting it. Without the block,
            // each goal is a no-op shell; the generic monster set (melee,
            // retaliate, wander) is the honest remainder. Its promotion goes
            // HERE when infested blocks land.
            default: break;
        }

        switch (def->base) {
            case MobBase::Monster:
                return std::make_unique<GenericMonster>(type, level);
            case MobBase::Animal:
                return std::make_unique<GenericAnimal>(type, level);
            case MobBase::PathfinderMob:
                return std::make_unique<GenericPathfinderMob>(type, level);
            case MobBase::Mob:
            default:
                return std::make_unique<GenericMob>(type, level);
        }
    }

} // namespace Game
