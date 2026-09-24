// File: src/server/sound/ServerSoundBroadcaster.cpp
#include "server/sound/ServerSoundBroadcaster.hpp"

#include "common/core/Assert.hpp"
#include "common/entity/Entity.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/SoundPackets.hpp"
#include "common/sound/SoundEvents.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <string>
#include <thread>
#include <utility>

namespace Server {

    std::optional<uint32_t> ServerSoundBroadcaster::ExceptPlayerId(const Game::SoundExcept& except) {
        if (except.player) {
            if (const auto* sp = dynamic_cast<const ServerPlayer*>(except.player)) return sp->getPlayerId();
            return std::nullopt;
        }
        if (except.entity && except.entity->IsPlayer()) {
            if (const auto* view = dynamic_cast<const PlayerEntityView*>(except.entity)) {
                if (const ServerPlayer* sp = view->GetPlayer()) return sp->getPlayerId();
            }
        }
        return std::nullopt;
    }

    void ServerSoundBroadcaster::PlaySound(Game::DimensionId dimension, const Game::SoundExcept& except,
                                           const glm::dvec3& pos, std::string_view event,
                                           Game::SoundSource source, float volume, float pitch,
                                           int64_t seed) {
        if (event.empty()) return;   // SoundEvents.EMPTY

        Network::SoundS2CPacket packet;
        packet.event  = std::string(event);
        packet.source = source;
        packet.SetPosition(pos);
        packet.volume = volume;
        packet.pitch  = pitch;
        packet.seed   = seed;

        Outgoing out;
        out.dimension      = dimension;
        out.exceptPlayerId = ExceptPlayerId(except);
        out.pos            = pos;
        out.range          = Game::SoundEvents::GetRange(volume);
        out.packetId       = static_cast<uint8_t>(Network::PacketId::SoundS2C);
        out.payload        = Network::Serialization::Serialize(packet);
        Submit(std::move(out));
    }

    void ServerSoundBroadcaster::PlaySoundFromEntity(Game::DimensionId dimension,
                                                     const Game::SoundExcept& except,
                                                     int32_t entityId, const glm::dvec3& pos,
                                                     std::string_view event, Game::SoundSource source,
                                                     float volume, float pitch, int64_t seed) {
        if (event.empty()) return;

        Network::SoundEntityS2CPacket packet;
        packet.event    = std::string(event);
        packet.source   = source;
        packet.entityId = entityId;
        packet.volume   = volume;
        packet.pitch    = pitch;
        packet.seed     = seed;

        Outgoing out;
        out.dimension      = dimension;
        out.exceptPlayerId = ExceptPlayerId(except);
        out.pos            = pos;
        out.range          = Game::SoundEvents::GetRange(volume);
        out.packetId       = static_cast<uint8_t>(Network::PacketId::SoundEntityS2C);
        out.payload        = Network::Serialization::Serialize(packet);
        Submit(std::move(out));
    }

    void ServerSoundBroadcaster::Submit(Outgoing&& out) {
        if (std::this_thread::get_id() == g_serverThreadId) {
            Broadcast(out);
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queued.push_back(std::move(out));
    }

    void ServerSoundBroadcaster::Flush() {
        std::vector<Outgoing> queued;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queued.empty()) return;
            queued.swap(m_queued);
        }
        for (const Outgoing& out : queued) Broadcast(out);
    }

    void ServerSoundBroadcaster::Broadcast(const Outgoing& out) const {
        if (!m_sessions) return;
        // MC PlayerList.broadcast: same dimension, strictly inside the range
        // sphere around the player's feet, never the excepted player.
        const double rangeSq = out.range * out.range;
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session) continue;
            const ServerPlayer* player = session->GetPlayer();
            ServerConnection* connection = session->GetConnection();
            if (!player || !connection) continue;
            if (out.exceptPlayerId && player->getPlayerId() == *out.exceptPlayerId) continue;
            if (Game::DimensionFromRaw(player->getDimensionId()) != out.dimension) continue;
            const glm::dvec3 d = out.pos - player->getPosition();
            if (d.x * d.x + d.y * d.y + d.z * d.z >= rangeSq) continue;
            connection->SendPacketIn(out.dimension, out.packetId, out.payload);
        }
    }

} // namespace Server
