// File: src/common/entity/HorseTaming.hpp
//
// MC AbstractHorse's taming half — temper, the tamed flag, the owner and the
// player seat taming happens in — shared by the two class trees that are
// equines in MC: AbstractHorse (horse, donkey, mule, skeleton and zombie
// horse) and Llama (+ TraderLlama), which this port keeps as a separate class
// (MC's Llama extends AbstractChestedHorse extends AbstractHorse). A mixin
// with a back-pointer to its Mob half, like TamableAnimal and NeutralMob.
//
// The taming loop, MC's:
//   * feeding raises temper (each class's handleEating);
//   * a click with an empty (or unusable) hand on an adult, unridden equine
//     seats the player (doPlayerRide → StartPlayerRiding);
//   * RunAroundLikeCrazyGoal bolts while an UNTAMED equine carries a player
//     and, on a 1-in-50 tick, rolls nextInt(maxTemper) < temper: tame
//     (tameWithName — owner, tamed, the hearts) or temper + 5, throw the
//     rider (ejectPassengers), makeMad and the smoke (entity event 6).
//
// Saved as MC does: "Tame", "Temper", "Owner" (EntityNbt).
#pragma once

#include "common/core/EntityRef.hpp"
#include "common/entity/PlayerRideable.hpp"

#include <cstdint>

namespace Game {

    class Mob;
    class LivingEntity;
    class JavaRandom;

    class HorseTaming : public PlayerRideable {
    public:
        explicit HorseTaming(Mob* self) : m_horseSelf(self) {}
        ~HorseTaming() override = default;

        // ── MC isTamed / setTamed (DATA_ID_FLAGS FLAG_TAME) ────────────────
        virtual bool IsTamed() const { return m_tamed; }
        void SetTamed(bool tamed) { m_tamed = tamed; }

        // ── MC temper ──────────────────────────────────────────────────────
        int  GetTemper() const { return m_temper; }
        void SetTemper(int temper) { m_temper = temper; }
        // MC modifyTemper: clamp(temper + amount, 0, getMaxTemper()).
        int  ModifyTemper(int amount);
        // MC getMaxTemper: 100 (Llama: 30).
        virtual int GetMaxTemper() const { return 100; }

        // ── MC owner (EntityReference<LivingEntity>, saved as "Owner") ─────
        const Uuid&      GetOwnerUuid() const { return m_ownerRef.GetUuid(); }
        void             SetOwnerUuid(const Uuid& uuid) { m_ownerRef.SetUnresolved(uuid); }
        const EntityRef& OwnerRef() const { return m_ownerRef; }
        void             SetOwner(const LivingEntity* owner);
        LivingEntity*    GetOwner() const;
        void             ClearOwnerReferenceTo(const Entity* e) { m_ownerRef.OnEntityRemoved(e); }

        // MC tameWithName(player): owner, tamed, the TAME_ANIMAL trigger (no
        // advancements here) and entity event 7 (the hearts).
        bool TameWithName(const LivingEntity& player);

        // MC makeMad — rear (where the kind can) and the angry sound.
        virtual void MakeMad() = 0;

        // MC isVehicle(): any passenger — a rider's view is in the list.
        bool IsEquineVehicle() const;
        // MC getFirstPassenger() instanceof Player — the rider's view.
        LivingEntity* GetPlayerRider() const;
        // A player sits on it (the first passenger is a player).
        bool HasPlayerRider() const { return GetPlayerRider() != nullptr; }
        // MC hasExactlyOnePlayerPassenger.
        bool HasExactlyOnePlayerPassenger() const;
        // MC ejectPassengers for the rider: thrown off (the riding system
        // puts them down beside the mount).
        void EjectPlayerRider();

        // MC doPlayerRide's startRiding half: seat `player` on this mob
        // (server only — the level owns the riding link). The caller has
        // already cleared the eating/standing flags.
        bool StartPlayerRide(LivingEntity& player);

        // MC handleEntityEvent 7 / 6 for an equine: spawnTamingParticles
        // (the hearts / the smoke). Client side.
        void SpawnTamingParticles(bool success) const;

        // MC Player.isSecondaryUseActive for a clicking player: sneaking.
        static bool IsSecondaryUseActive(const LivingEntity& player);

        // MC AbstractHorse.getDismountLocationForPassenger: beside the mount
        // on the rider's main-hand side, else the off-hand side (each tried
        // up to 0.75 above the mount's top for every dismount pose), else
        // the mount's own position.
        static glm::dvec3 EquineDismountLocation(const Mob& mount, const LivingEntity& passenger);

        // ── MC AbstractHorse's attribute generators and ranges ─────────────
        // generateMaxHealth(random::nextInt): 15 + nextInt(8) + nextInt(9).
        static float  GenerateMaxHealth(JavaRandom& rng);
        // generateJumpStrength / generateSpeed with random::nextDouble.
        static double GenerateJumpStrength(JavaRandom& rng);
        static double GenerateSpeed(JavaRandom& rng);
        // MC AbstractHorse.createOffspringAttribute: the parents' average
        // plus a triangular spread over |a - b| + 30 % of the range, folded
        // back inside [min, max].
        static double CreateOffspringAttribute(double parentA, double parentB, double min, double max,
                                               JavaRandom& rng);
        // MC AbstractHorse.setOffspringAttributes(partner, baby): MAX_HEALTH,
        // JUMP_STRENGTH, MOVEMENT_SPEED (in that order — the RNG stream) from
        // both parents' BASE values within the MIN_/MAX_ ranges.
        static void SetOffspringAttributes(const Mob& self, const Mob& partner, Mob& baby);

    protected:
        Mob* HorseSelf() const { return m_horseSelf; }

    private:
        Mob*      m_horseSelf;
        bool      m_tamed = false;
        int       m_temper = 0;
        EntityRef m_ownerRef;
    };

} // namespace Game
