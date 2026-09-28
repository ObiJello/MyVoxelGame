// File: src/common/world/level/gameevent/GameEventLevel.cpp
//
// The emit side of the game-event system: MC LevelAccessor.gameEvent's
// overloads on ILevelWrite, and Entity.gameEvent. Both forward to the level's
// GameEventDispatcher, which only the server's World has.
#include "GameEvent.hpp"
#include "GameEventDispatcher.hpp"

#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/level/ILevelWrite.hpp"

namespace Game {

    void ILevelWrite::GameEvent(GameEventId event, const glm::dvec3& pos, const GameEventContext& context) {
        if (GameEventDispatcher* dispatcher = GameEvents()) dispatcher->Post(event, pos, context);
    }

    void ILevelWrite::GameEvent(GameEventId event, const glm::ivec3& pos, const GameEventContext& context) {
        // Vec3.atCenterOf(pos).
        GameEvent(event, glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5), context);
    }

    void ILevelWrite::GameEvent(Entity* sourceEntity, GameEventId event, const glm::dvec3& pos) {
        GameEvent(event, pos, GameEventContext::Of(sourceEntity));
    }

    void ILevelWrite::GameEvent(Entity* sourceEntity, GameEventId event, const glm::ivec3& pos) {
        GameEvent(event, pos, GameEventContext::Of(sourceEntity));
    }

    void Entity::GameEvent(GameEventId event, Entity* sourceEntity) {
        // MC: level().gameEvent(sourceEntity, event, this.position).
        if (!m_level || m_level->IsClientSide()) return;
        if (ILevelWrite* level = m_level->MutableBlocks()) level->GameEvent(sourceEntity, event, position);
    }

    bool Entity::EmitsMovementEvents() const {
        // MC getMovementEmission() == NONE for exactly these; everything else
        // (ALL, or EVENTS for bats, squids, guardians, silverfish, endermites,
        // breezes, boats and minecarts) emits the events.
        switch (GetType()) {
            case EntityTypeId::Arrow:
            case EntityTypeId::Trident:
            case EntityTypeId::FallingBlock:
            case EntityTypeId::Tnt:
            case EntityTypeId::EndCrystal:
            case EntityTypeId::Shulker:
            case EntityTypeId::FishingBobber:
                return false;
            default:
                return true;
        }
    }

} // namespace Game
