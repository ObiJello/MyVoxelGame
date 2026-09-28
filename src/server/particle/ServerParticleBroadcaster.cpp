// File: src/server/particle/ServerParticleBroadcaster.cpp
#include "server/particle/ServerParticleBroadcaster.hpp"

#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/LevelParticlePackets.hpp"
#include "server/IntegratedServer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include <cmath>
#include <thread>
#include <utility>

namespace Server {

    int ServerParticleBroadcaster::SendParticles(Game::DimensionId dimension,
                                                 const Game::Particles::ParticleBurst& burst) {
        Network::LevelParticlesS2CPacket packet;
        packet.options         = burst.options;
        packet.overrideLimiter = burst.overrideLimiter;
        packet.alwaysShow      = burst.alwaysShow;
        packet.pos             = burst.pos;
        packet.dist            = burst.dist;
        packet.maxSpeed        = burst.maxSpeed;
        packet.count           = burst.count;
        packet.randomization   = burst.randomization;

        Outgoing out;
        out.dimension       = dimension;
        out.pos             = burst.pos;
        out.overrideLimiter = burst.overrideLimiter;
        out.payload         = Network::Serialization::Serialize(packet);

        if (std::this_thread::get_id() == g_serverThreadId) return Broadcast(out);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queued.push_back(std::move(out));
        return 0;
    }

    void ServerParticleBroadcaster::Flush() {
        std::vector<Outgoing> queued;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queued.empty()) return;
            queued.swap(m_queued);
        }
        for (const Outgoing& out : queued) Broadcast(out);
    }

    int ServerParticleBroadcaster::Broadcast(const Outgoing& out) const {
        if (!m_sessions) return 0;
        // MC ServerLevel.sendParticles(player, overrideLimiter, x, y, z, packet):
        // the player's level, and player.blockPosition().closerToCenterThan(
        // pos, overrideLimiter ? 512 : 32) — measured from the centre of the
        // block the player stands in.
        const double range = out.overrideLimiter ? 512.0 : 32.0;
        const double rangeSq = range * range;
        int sent = 0;
        for (const auto& session : m_sessions->GetAllSessions()) {
            if (!session) continue;
            const ServerPlayer* player = session->GetPlayer();
            ServerConnection* connection = session->GetConnection();
            if (!player || !connection) continue;
            if (Game::DimensionFromRaw(player->getDimensionId()) != out.dimension) continue;
            const glm::dvec3 feet = player->getPosition();
            const glm::dvec3 centre(std::floor(feet.x) + 0.5, std::floor(feet.y) + 0.5, std::floor(feet.z) + 0.5);
            const glm::dvec3 d = out.pos - centre;
            if (d.x * d.x + d.y * d.y + d.z * d.z >= rangeSq) continue;
            connection->SendPacketIn(out.dimension, static_cast<uint8_t>(Network::PacketId::LevelParticlesS2C),
                                     out.payload);
            ++sent;
        }
        return sent;
    }

} // namespace Server
