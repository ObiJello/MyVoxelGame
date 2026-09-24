// File: src/server/commands/CommandSourceStack.cpp
#include "CommandSourceStack.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/core/Mth.hpp"

namespace Server {

    CommandSourceStack CommandSourceStack::ForPlayer(ServerPlayer& sender, PlayerSessionManager& sessions) {
        CommandSourceStack s;
        s.sender   = &sender;
        s.sessions = &sessions;
        s.position = sender.getPosition();
        // The session's dimension — selectors are level-scoped (see
        // CommandSource); the player's own field is the fallback.
        if (auto session = sessions.GetSession(sender.getPlayerId())) {
            s.dimension = Game::DimensionFromRaw(session->GetDimensionId());
        } else {
            s.dimension = Game::DimensionFromRaw(sender.getDimensionId());
        }
        s.rotation = CommandRotation{ sender.getYaw(), sender.getPitch() };
        SelectedEntity self;
        if (DescribePlayer(sender, s, self)) s.entity = std::move(self);
        s.anchorEyes = false;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithEntity(const SelectedEntity& entity) const {
        CommandSourceStack s = *this;
        s.entity = entity;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithPosition(const glm::dvec3& position) const {
        CommandSourceStack s = *this;
        s.position = position;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithRotation(const CommandRotation& rot) const {
        CommandSourceStack s = *this;
        s.rotation = rot;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithLevel(Game::DimensionId dimension) const {
        CommandSourceStack s = *this;
        s.dimension = dimension;
        return s;
    }

    CommandSourceStack CommandSourceStack::WithAnchor(bool eyes) const {
        CommandSourceStack s = *this;
        s.anchorEyes = eyes;
        return s;
    }

    CommandSourceStack CommandSourceStack::Facing(const glm::dvec3& target) const {
        // MC: dx/dy/dz from the anchor, xRot = -atan2(dy, horizontal),
        // yRot = atan2(dz, dx) - 90 — the same angles Mth's helpers derive.
        const glm::vec3 d(target - AnchorPosition());
        return WithRotation(CommandRotation{ Game::Mth::YRotFromVector(d), Game::Mth::XRotFromVector(d) });
    }

    CommandSourceStack CommandSourceStack::Facing(const SelectedEntity& entity, bool eyes) const {
        return Facing(EntityAnchorPosition(entity, eyes));
    }

} // namespace Server
