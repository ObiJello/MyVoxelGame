// File: src/common/core/EntityRef.cpp
#include "common/core/EntityRef.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"

namespace Game {

    void EntityRef::Set(const Entity* entity) {
        if (!entity) { Clear(); return; }
        m_uuid     = entity->GetUuid();
        m_resolved = const_cast<Entity*>(entity);
        entity->EnsureLivenessToken();
        m_alive    = entity->LivenessToken();
    }

    bool EntityRef::Matches(const Entity& entity) const {
        return !Empty() && entity.GetUuid() == m_uuid;
    }

    Entity* EntityRef::Get(EntityLevel& level) {
        // A cached pointer is only good while the object still EXISTS (its
        // liveness token — checked without dereferencing it) and the entity
        // is still live.
        if (m_resolved && !m_alive.expired() && !m_resolved->IsRemoved()) return m_resolved;
        m_resolved = nullptr;
        m_alive.reset();

        if (Empty()) return nullptr;

        // This level and its siblings first, then the player list — MC splits
        // the same way (ServerLevel.getEntityInAnyDimension vs
        // PlayerList.getPlayer).
        Entity* found = level.ResolveEntity(m_uuid);
        if (!found) found = level.ResolvePlayer(m_uuid);
        if (found) {
            found->EnsureLivenessToken();
            m_resolved = found;
            m_alive    = found->LivenessToken();
        }

        // m_uuid is deliberately untouched on failure: the referent may simply
        // be in a chunk that is not loaded, and it may come back.
        return found;
    }

    LivingEntity* EntityRef::GetLiving(EntityLevel& level) {
        return dynamic_cast<LivingEntity*>(Get(level));
    }

    void EntityRef::OnEntityRemoved(const Entity* entity) {
        if (!entity || m_resolved != entity) return;
        m_resolved = nullptr;
        m_alive.reset();

        const RemovalReason reason = entity->GetRemovalReason();
        if (reason == RemovalReason::Killed || reason == RemovalReason::Discarded) {
            Clear();     // genuinely gone — forget it
        }
        // UnloadedToChunk / ChangedDimension: keep the identity, resolve later.
    }

} // namespace Game
