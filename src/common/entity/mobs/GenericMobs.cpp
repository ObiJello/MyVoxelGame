// File: src/common/entity/mobs/GenericMobs.cpp
#include "common/entity/mobs/GenericMobs.hpp"

#include "common/entity/mobs/AnimatedMobs.hpp"

#include "common/entity/ai/goals/AnimalGoals.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
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

        // MC picks between the two stroll goals per mob — 8 of them use the
        // plain RandomStrollGoal, which will walk into water. They are
        // different goals, so the choice is carried in the def.
        void AddStroll(PathfinderMob& mob, GoalSelector& goals,
                       const MobDef& def, int priority) {
            if (def.strollSpeed <= 0.0) return;
            if (def.strollAvoidsWater) {
                goals.AddGoal(priority, std::make_unique<WaterAvoidingRandomStrollGoal>(
                                            &mob, def.strollSpeed));
            } else {
                goals.AddGoal(priority, std::make_unique<RandomStrollGoal>(
                                            &mob, def.strollSpeed));
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

    // ── Monster ────────────────────────────────────────────────────────────

    GenericMonster::GenericMonster(EntityTypeId type, EntityLevel* level)
        : Monster(type, level) {
        CreateMonsterAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
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

    // ── Animal ─────────────────────────────────────────────────────────────

    GenericAnimal::GenericAnimal(EntityTypeId type, EntityLevel* level)
        : Animal(type, level) {
        CreateAnimalAttributes(m_attributes);
        ApplyDef(m_attributes, DefFor(type));
        SetLandSpeedFactor(DefFor(type).landSpeedFactor);
        SetWalkAnimParams(DefFor(type).walkAnimScale, DefFor(type).walkAnimCap,
                          DefFor(type).walkAnimFactor, DefFor(type).walkAnimBabyScale);
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
        // Same type as the parent. AgeableMob's own breeding path sets the
        // baby age, exactly as the hand-written Cow/Pig/Sheep do.
        return std::make_unique<GenericAnimal>(GetType(), m_level);
    }

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
            case EntityTypeId::Frog:      return std::make_unique<Frog>(level);
            case EntityTypeId::Camel:     return std::make_unique<Camel>(level);
            // MC's CamelHusk extends Camel — same brain, same pose machinery.
            case EntityTypeId::CamelHusk:
                return std::make_unique<Camel>(level, EntityTypeId::CamelHusk);
            case EntityTypeId::Bat:       return std::make_unique<Bat>(level);
            case EntityTypeId::Armadillo: return std::make_unique<Armadillo>(level);
            case EntityTypeId::Tadpole:   return std::make_unique<Tadpole>(level);
            case EntityTypeId::Goat:      return std::make_unique<Goat>(level);
            case EntityTypeId::Hoglin:    return std::make_unique<Hoglin>(level);
            case EntityTypeId::Warden:    return std::make_unique<Warden>(level);
            case EntityTypeId::Creaking:  return std::make_unique<Creaking>(level);
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
