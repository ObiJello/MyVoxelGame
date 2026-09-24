// File: src/server/control/RemoteControlManager.cpp
#include "RemoteControlManager.hpp"

#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/core/Log.hpp"

namespace Server {

    namespace {
        std::shared_ptr<PlayerSession> LiveSession(PlayerSessionManager& sessions, uint32_t id) {
            auto s = sessions.GetSession(id);
            if (!s || !s->GetPlayer() || !s->GetConnection()) return nullptr;
            return s;
        }
    }

    uint32_t RemoteControlManager::TargetOf(uint32_t controllerId) const {
        for (const Pair& p : m_pairs) if (p.controller == controllerId) return p.target;
        return 0;
    }

    uint32_t RemoteControlManager::ControllerOf(uint32_t targetId) const {
        for (const Pair& p : m_pairs) if (p.target == targetId) return p.controller;
        return 0;
    }

    void RemoteControlManager::SendRole(uint32_t toId, uint8_t role, uint32_t otherId,
                                        const std::string& otherName) {
        auto s = LiveSession(m_sessions, toId);
        if (!s) return;
        Network::ControlS2CPacket out;
        out.role      = static_cast<Network::ControlRole>(role);
        out.otherId   = otherId;
        out.otherName = otherName;
        s->GetConnection()->SendPacket(static_cast<uint8_t>(Network::PacketId::ControlS2C),
                                       Network::Serialization::Serialize(out));
    }

    bool RemoteControlManager::Start(uint32_t controllerId, uint32_t targetId, std::string& error) {
        if (controllerId == targetId) { error = "You cannot control yourself"; return false; }
        auto controller = LiveSession(m_sessions, controllerId);
        auto target     = LiveSession(m_sessions, targetId);
        if (!controller || !target) { error = "Player not found"; return false; }
        if (TargetOf(controllerId) || ControllerOf(controllerId)) {
            error = "You are already in a control session (/control off first)";
            return false;
        }
        if (TargetOf(targetId) || ControllerOf(targetId)) {
            error = target->GetPlayer()->getName() + " is already in a control session";
            return false;
        }
        if (controller->GetDimensionId() != target->GetDimensionId()) {
            error = target->GetPlayer()->getName() + " is in another dimension";
            return false;
        }

        m_pairs.push_back({controllerId, targetId});
        target->GetConnection()->SetRelayViewTo(controllerId);
        // The tee: what the server tells the controlled player about their
        // inventory, containers and stats, the controller hears too.
        target->GetConnection()->SetMirrorPlayerId(controllerId);
        controller->GetConnection()->SetRelayInputTo(targetId);
        SendRole(controllerId, static_cast<uint8_t>(Network::ControlRole::Controller),
                 targetId, target->GetPlayer()->getName());
        SendRole(targetId, static_cast<uint8_t>(Network::ControlRole::Controlled),
                 controllerId, controller->GetPlayer()->getName());
        // The controller's client starts from the controlled player's
        // current inventory and stats, not from a diff.
        target->SendInventoryFull();
        target->ResendStats();
        controller->FollowAnchor(target->GetChunkPosition());
        Log::Info("[Control] %s now controls %s",
                  controller->GetPlayer()->getName().c_str(), target->GetPlayer()->getName().c_str());
        return true;
    }

    void RemoteControlManager::End(const Pair& pair, const std::string& reason) {
        auto controller = LiveSession(m_sessions, pair.controller);
        auto target     = LiveSession(m_sessions, pair.target);
        if (target) {
            target->GetConnection()->SetMirrorPlayerId(0);
            target->GetConnection()->SetRelayViewTo(0);
            // No word to the controlled player, at the start or the end:
            // they are not told.
            SendRole(pair.target, static_cast<uint8_t>(Network::ControlRole::None), pair.controller, "");
        }
        if (controller) {
            controller->GetConnection()->SetRelayInputTo(0);
            SendRole(pair.controller, static_cast<uint8_t>(Network::ControlRole::None), pair.target, "");
            if (!reason.empty()) controller->SendSystemMessage(reason);
            // Their own inventory and stats again — the controlled player's
            // were teed over these for the whole session.
            controller->SendInventoryFull();
            controller->ResendStats();
            controller->GetConnection()->SendPlayerAbilities(*controller->GetPlayer());
            controller->ResetAnchor();
        }
        Log::Info("[Control] pair %u -> %u ended: %s", pair.controller, pair.target, reason.c_str());
    }

    void RemoteControlManager::Stop(uint32_t playerId, const std::string& reason) {
        for (size_t i = 0; i < m_pairs.size(); ++i) {
            if (m_pairs[i].controller == playerId || m_pairs[i].target == playerId) {
                const Pair pair = m_pairs[i];
                m_pairs.erase(m_pairs.begin() + static_cast<std::ptrdiff_t>(i));
                End(pair, reason);
                return;
            }
        }
    }

    void RemoteControlManager::Tick() {
        for (size_t i = 0; i < m_pairs.size();) {
            const Pair pair = m_pairs[i];
            auto controller = LiveSession(m_sessions, pair.controller);
            auto target     = LiveSession(m_sessions, pair.target);
            std::string reason;
            if (!controller || !target)                                  reason = "Control ended: player left";
            else if (controller->GetDimensionId() != target->GetDimensionId())
                                                                         reason = "Control ended: different dimensions";
            if (!reason.empty()) {
                m_pairs.erase(m_pairs.begin() + static_cast<std::ptrdiff_t>(i));
                End(pair, reason);
                continue;
            }
            // MC ServerPlayer.tick for a spectator camera: the tracked
            // position is the camera's, every tick.
            controller->FollowAnchor(target->GetChunkPosition());
            ++i;
        }
    }

} // namespace Server
