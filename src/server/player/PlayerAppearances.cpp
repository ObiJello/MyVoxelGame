// File: src/server/player/PlayerAppearances.cpp
#include "server/player/PlayerAppearances.hpp"

#include "server/IntegratedServer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/entity/PlayerAppearance.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/PlayerAppearancePackets.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace Server::PlayerAppearances {

    namespace {

        // Player id → the look as relayed (validated). Tick thread only.
        std::unordered_map<uint32_t, Game::PlayerAppearance>& Store() {
            static std::unordered_map<uint32_t, Game::PlayerAppearance> s_store;
            return s_store;
        }

        std::vector<uint8_t> Encode(uint32_t playerId, const Game::PlayerAppearance& appearance) {
            Network::PlayerAppearanceS2CPacket packet;
            packet.playerId = playerId;
            packet.appearance = appearance;
            return Network::Serialization::Serialize(packet);
        }

        PlayerSessionManager* Sessions() {
            auto* server = g_integratedServer.get();
            return server ? server->GetSessionManager() : nullptr;
        }

    } // namespace

    void HandleC2S(PlayerSession& session, const Network::PlayerAppearanceC2SPacket& packet) {
        const uint32_t playerId = session.GetPlayerId();
        Game::PlayerAppearance appearance = packet.appearance;
        std::vector<std::string> dropped;
        appearance.Sanitize(&dropped);
        for (const std::string& line : dropped) {
            Log::Warning("[PlayerAppearances] player %u: %s", playerId, line.c_str());
        }

        auto& store = Store();
        const auto it = store.find(playerId);
        // A client resending what it already sent changes nothing for anyone.
        if (it != store.end() && it->second == appearance) return;
        store[playerId] = appearance;

        Log::Info("[PlayerAppearances] player %u: %s%s%s (skin %zu B, cape %zu B, drawing %zu B)", playerId,
                  Game::AppearanceModeSlug(appearance.mode),
                  appearance.IsSkin() ? " " : "",
                  appearance.IsSkin() ? Game::SkinModelSlug(appearance.model)
                                      : (appearance.IsDrawn() ? " drawn" : appearance.hasPaint ? " painted" : ""),
                  appearance.skinPng.size(), appearance.capePng.size(),
                  appearance.drawing.EncodedSize());

        PlayerSessionManager* sessions = Sessions();
        if (!sessions) return;
        const std::vector<uint8_t> data = Encode(playerId, appearance);
        for (const auto& other : sessions->GetAllSessions()) {
            if (!other || other->GetPlayerId() == playerId) continue;
            if (ServerConnection* connection = other->GetConnection()) {
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAppearanceS2C), data);
            }
        }
    }

    void SyncOnJoin(ServerConnection& joining, uint32_t joinerId) {
        PlayerSessionManager* sessions = Sessions();
        if (!sessions) return;
        const auto& store = Store();
        for (const auto& session : sessions->GetAllSessions()) {
            if (!session || session->GetPlayerId() == joinerId) continue;
            const auto it = store.find(session->GetPlayerId());
            if (it == store.end()) continue;   // a player who has not sent one: the stick figure
            joining.SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAppearanceS2C),
                               Encode(it->first, it->second));
        }
    }

    void Forget(uint32_t playerId) {
        Store().erase(playerId);
    }

    void Clear() {
        Store().clear();
    }

} // namespace Server::PlayerAppearances
