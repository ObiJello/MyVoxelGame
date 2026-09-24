// File: src/server/control/RemoteControlManager.hpp
//
// The server half of `/control <player>` (see ControlPackets.hpp for the
// model). It keeps the controller/controlled pairs and does three things:
//
//   • relays: the controller's ControlInput frames go to the controlled
//     client, the controlled client's ControlView frames go back — set up
//     here, carried out inline on the I/O thread by ServerConnection
//     (SetRelayInputTo / SetRelayViewTo), never through the tick;
//   • tees: while a pair stands, the controlled player's inventory,
//     container and stat packets are also sent to the controller
//     (ServerConnection's mirror), so the controller's client holds the
//     same inventory the controlled one draws;
//   • follows: the controller's chunk-tracking anchor is pinned to the
//     controlled player's chunk every tick — MC ServerPlayer.tick does the
//     same for a spectator's camera (absSnapTo + chunkSource.move) — so the
//     controller's client is streamed the terrain around the view it shows.
//
// A pair ends when either side asks (`/control off`), leaves, dies out of
// the session list, or the two stop sharing a dimension. Server thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Server {

    class PlayerSessionManager;

    class RemoteControlManager {
    public:
        explicit RemoteControlManager(PlayerSessionManager& sessions) : m_sessions(sessions) {}

        // `error` is the message for the sender when this returns false.
        bool Start(uint32_t controllerId, uint32_t targetId, std::string& error);
        // Ends whichever pair `playerId` is part of, on either side. No-op
        // when there is none.
        void Stop(uint32_t playerId, const std::string& reason);
        // Per-tick: drop dead pairs, follow the anchor.
        void Tick();

        // 0 when not part of a pair in that role.
        uint32_t TargetOf(uint32_t controllerId) const;
        uint32_t ControllerOf(uint32_t targetId) const;

    private:
        struct Pair { uint32_t controller = 0; uint32_t target = 0; };
        void End(const Pair& pair, const std::string& reason);
        void SendRole(uint32_t toId, uint8_t role, uint32_t otherId, const std::string& otherName);

        PlayerSessionManager& m_sessions;
        std::vector<Pair>     m_pairs;
    };

} // namespace Server
