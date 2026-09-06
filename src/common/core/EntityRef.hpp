// File: src/common/core/EntityRef.hpp
//
// A reference to another entity that survives a save.
//
// Port of net.minecraft.world.entity.EntityReference, which is an
// Either<UUID, Entity>: an identity that may or may not currently be resolved
// to a live object. Every cross-entity reference the engine persists — a tamed
// pet's owner, a projectile's shooter, who last hurt me, who I am angry at —
// goes through this.
//
// WHY NOT A RAW POINTER. A pointer cannot be written to disk, and the obvious
// alternative (write the entity id) is worse than useless: ids are per-level
// and per-session, reset on every launch, so a saved id names a different
// entity or none at all.
//
// WHY NOT A TWO-PASS BINDER. Because the referent is usually NOT in the chunk
// being loaded — it is in another chunk, another dimension, or offline. MC
// resolves lazily on first use and keeps the UUID forever if that fails
// (EntityReference.java:53-74), so a cow can round-trip a grudge against a
// player who has not logged in for a month. An eager pass would resolve a
// minority of references and still need the lazy path as a fallback; this is
// one mechanism instead of two.
#pragma once

#include "common/core/Uuid.hpp"

namespace Game {

    class Entity;
    class LivingEntity;
    struct EntityLevel;

    class EntityRef {
    public:
        bool        Empty()   const { return UuidIsNil(m_uuid); }
        const Uuid& GetUuid() const { return m_uuid; }

        // From a save: identity only, never a pointer.
        void SetUnresolved(const Uuid& uuid) { m_uuid = uuid; m_resolved = nullptr; }

        // From gameplay: stamps both halves.
        void Set(const Entity* entity);
        void Clear() { m_uuid = Uuid{}; m_resolved = nullptr; }

        // Identity comparison that never triggers a resolve — for "is this the
        // entity I am angry at?" tests on the hot path.
        bool Matches(const Entity& entity) const;

        // Lazy resolve. May return null; the UUID is NEVER cleared by a failed
        // lookup, so the next call tries again.
        Entity*       Get(EntityLevel& level);
        LivingEntity* GetLiving(EntityLevel& level);

        // The referent went away. Always demote the pointer; drop the identity
        // too ONLY if it actually died — an entity that merely unloaded or
        // changed dimension still exists and must stay referenceable.
        void OnEntityRemoved(const Entity* entity);

    private:
        Uuid    m_uuid{};
        Entity* m_resolved = nullptr;
    };

} // namespace Game
