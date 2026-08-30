// File: src/common/entity/TamableAnimal.hpp
//
// MC net.minecraft.world.entity.TamableAnimal — tame flag, owner, sitting.
//
// MC makes this a class between Animal and Wolf/Cat/Parrot; here it is a
// mixin base with a back-pointer to the Mob half of the object, exactly the
// NeutralMob pattern — because Wolf sits on GenericAnimal (for the def's
// attributes and goal set) and a second Animal in its bases would be a
// diamond. Every implementer derives from both, and the renderer finds the
// mixin with one dynamic_cast (MobRenderer's TamableAnimal block).
//
// The owner is stored as the player's ENTITY ID (= connection id) rather than
// MC's EntityReference UUID: this engine has no mob NBT persistence, so an
// owner reference that survives save/load has nothing to survive in — and
// within a session the entity id IS the stable player identity. A tamed mob
// whose owner disconnects simply resolves no owner until they return with the
// same connection id; the tame flag itself stays.
//
// Wire sync: MC's DATA_FLAGS_ID byte (bit 0 sitting-in-pose, bit 2 tame) is
// carried on each implementer's anim state byte via GetTamableAnimByte —
// bit 0 = sitting pose, bit 1 = tame, MC's meanings at this port's packing.
// orderedToSit is server-only in MC too (it is saved, not synched).
#pragma once

#include "common/core/EntityRef.hpp"

#include <cstdint>

#include "common/entity/Item.hpp"

namespace Game {

    class Mob;
    class LivingEntity;
    class Entity;

    class TamableAnimal {
    public:
        // MC TamableAnimal.TELEPORT_WHEN_DISTANCE_IS_SQ.
        static constexpr double kTeleportWhenDistanceIsSq = 144.0;

        virtual ~TamableAnimal() = default;

        // ── Tame flag (MC isTame / setTame) ────────────────────────────────
        bool IsTame() const { return m_tame; }
        void SetTame(bool tame, bool includeSideEffects) {
            m_tame = tame;
            if (includeSideEffects) ApplyTamingSideEffects();
        }

        // MC applyTamingSideEffects — the wolf's 40-health boost.
        virtual void ApplyTamingSideEffects() {}

        // ── Sitting (MC isInSittingPose / isOrderedToSit) ──────────────────
        // Two flags on purpose, exactly as MC keeps them: orderedToSit is the
        // COMMAND, the pose is the STATE SitWhenOrderedToGoal writes while it
        // runs (the goal refuses to sit mid-water, mid-air, or while the
        // nearby owner is being attacked).
        bool IsInSittingPose() const { return m_inSittingPose; }
        void SetInSittingPose(bool v) { m_inSittingPose = v; }
        bool IsOrderedToSit() const { return m_orderedToSit; }
        void SetOrderedToSit(bool v) { m_orderedToSit = v; }

        // ── Owner (entity id — see the header note) ────────────────────────
        // The owner is a UUID, not a session id.
        //
        // It was an int32_t entity id, which cannot survive a save: ids are
        // per-level and reset on every launch, so a tamed wolf would come back
        // owned by whatever happened to hold that number. The identity is kept
        // even when the owner is offline or in an unloaded chunk — that is
        // vanilla's behaviour, and it is why a wolf still knows its owner a
        // month later.
        const Uuid& GetOwnerUuid() const { return m_ownerRef.GetUuid(); }
        void        SetOwnerUuid(const Uuid& uuid) { m_ownerRef.SetUnresolved(uuid); }
        bool        HasOwner() const { return !m_ownerRef.Empty(); }
        const EntityRef& OwnerRef() const { return m_ownerRef; }
        void        ClearOwnerReferenceTo(const Entity* e) { m_ownerRef.OnEntityRemoved(e); }
        void    SetOwner(const LivingEntity* owner);

        // Resolves the owner among the level's current players; null when the
        // owner is offline or nothing was ever set.
        LivingEntity* GetOwner() const;
        bool IsOwnedBy(const LivingEntity& entity) const;

        // MC tame(player): flag + side effects + owner. (The TAME_ANIMAL
        // advancement trigger has no advancement system to land in.)
        void Tame(const LivingEntity& player);

        // MC TamableAnimal.canAttack's owner exemption — implementers call
        // this from their CanAttack override: an owned mob never targets its
        // owner.
        bool TamableCanAttack(const LivingEntity& target) const {
            return !IsOwnedBy(target);
        }

        // MC wantsToAttack — the wolf narrows it (no creepers/ghasts, no
        // tame animals); base says yes.
        virtual bool WantsToAttack(const LivingEntity& target,
                                   const LivingEntity& owner) const {
            (void)target; (void)owner;
            return true;
        }

        // ── Follow / teleport (MC's own methods, verbatim) ─────────────────
        // MC unableToMoveToOwner: ordered to sit, riding, (leashed — no leash
        // system), or the owner is a spectator.
        bool UnableToMoveToOwner() const;
        bool ShouldTryTeleportToOwner() const;
        void TryToTeleportToOwner();

        // MC canFlyToOwner — parrot true: it may teleport onto leaves.
        virtual bool CanFlyToOwner() const { return false; }

        // MC TamableAnimal.feed — consume one, heal by the food's nutrition
        // times the factor (defaultHeal when the item carries no FOOD
        // component). The eating sound waits on the sound system.
        void Feed(ItemStack& held, float healingFactor, float defaultHeal);

        // MC spawnTamingParticles via entity events 7 (hearts) / 6 (smoke).
        void BroadcastTamingResult(bool success);

        // MC TamableAnimal.spawnTamingParticles — 7 particles (HEART on
        // success, SMOKE on failure) with gaussian * 0.02 velocities at
        // random points on the body. CLIENT-side, run from each
        // implementer's HandleEntityEvent(7 / 6); the level's AddParticle
        // no-ops on the server exactly as MC's addParticle does.
        void SpawnTamingParticles(bool success);

        // MC TamableAnimal.handleEntityEvent — 7 = success hearts, 6 =
        // failure smoke. Returns true when the byte was consumed so each
        // implementer's HandleEntityEvent override can fall through to its
        // super for everything else (the mixin has no super of its own).
        bool HandleTamableEntityEvent(uint8_t id) {
            if (id == 7)      { SpawnTamingParticles(true);  return true; }
            else if (id == 6) { SpawnTamingParticles(false); return true; }
            return false;
        }

        // ── Wire byte (see the header note) ────────────────────────────────
        uint8_t GetTamableAnimByte() const {
            return static_cast<uint8_t>((m_inSittingPose ? 1 : 0) |
                                        (m_tame ? 2 : 0));
        }
        void SetTamableAnimByte(uint8_t v) {
            m_inSittingPose = (v & 1) != 0;
            m_tame = (v & 2) != 0;
        }

    protected:
        explicit TamableAnimal(Mob* self) : m_tamableSelf(self) {}

    private:
        // MC maybeTeleportTo / canTeleportTo.
        bool MaybeTeleportTo(int x, int y, int z);
        bool CanTeleportTo(int x, int y, int z) const;

        Mob*    m_tamableSelf;
        bool    m_tame = false;
        bool    m_orderedToSit = false;
        bool    m_inSittingPose = false;
        EntityRef m_ownerRef;
    };

} // namespace Game
