// File: src/common/entity/mobs/GenericMobs.hpp
//
// One class per BASE, not one per mob.
//
// The eight original mobs each have a hand-written class because each has
// behaviour worth porting exactly — a creeper's fuse, a sheep's grazing, a
// zombie's daylight burn. The other 77 do not have that yet, and writing 77
// near-identical classes to say so would be 77 places to keep in sync.
//
// So they share these three, driven by GeneratedMobDefs: the def supplies the
// attributes and the base, and the base supplies MC's default goal set for that
// kind of mob. What you get is a mob that walks, wanders, looks at you, takes
// and deals damage, drops loot, despawns and renders correctly — everything
// PathfinderMob/Monster/Animal already provide — and nothing bespoke.
//
// Promoting one to a hand-written class later is additive: add the class, add a
// row to MakeMob's switch, and the generic path stops being used for it. The
// generated attribute and mesh data stay valid either way.
#pragma once

#include "common/entity/Animal.hpp"
#include "common/entity/Monster.hpp"
#include "common/entity/GeneratedMobDefs.hpp"

namespace Game {

    // MC Mob — a mob with NO ground path navigation. In MC this is the swimmers,
    // the fliers and the sitters: fish, squid, bats, ghasts, phantoms, slimes,
    // shulkers, the ender dragon. None of them has a stroll goal in
    // registerGoals, and none of them is given MOVEMENT_SPEED in
    // DefaultAttributes — they sit at the registry default of 0.7, which is
    // more than three times a cow's 0.2.
    //
    // That combination is why they must NOT get the strolling set: a mob that
    // MC never intended to walk, walking, does it at triple speed. It floats
    // and looks around instead, pending real swim/fly navigation.
    class GenericMob : public Mob {
    public:
        GenericMob(EntityTypeId type, EntityLevel* level);

    protected:
        void RegisterGoals() override;
    };

    // MC PathfinderMob's own default: it wanders and looks around, and that is
    // all. Golems, villagers and allays land here.
    class GenericPathfinderMob : public PathfinderMob {
    public:
        GenericPathfinderMob(EntityTypeId type, EntityLevel* level);

    protected:
        void RegisterGoals() override;
    };

    // MC Monster's shape: melee the player, retaliate, wander otherwise.
    // Deliberately NOT ranged — nothing here can fire a projectile yet, so a
    // skeleton-alike that stood at range would simply never attack.
    class GenericMonster : public Monster {
    public:
        GenericMonster(EntityTypeId type, EntityLevel* level);

    protected:
        void RegisterGoals() override;
    };

    // MC Animal's shape: panic when hurt, follow food, breed, wander.
    class GenericAnimal : public Animal {
    public:
        GenericAnimal(EntityTypeId type, EntityLevel* level);

        // Generic animals have no breeding food of their own yet, so nothing
        // tempts them and CreateBaby returns another of the same type.
        bool IsFood(uint32_t itemId) const override;
        std::unique_ptr<Animal> CreateBaby() override;

    protected:
        void RegisterGoals() override;
    };

    // Build the right base for `type` from its generated def, with the def's
    // attributes applied. Returns null for a type that has no def — the eight
    // hand-written mobs, and anything that is not a mob.
    std::unique_ptr<Mob> MakeGenericMob(EntityTypeId type, EntityLevel* level);

} // namespace Game
