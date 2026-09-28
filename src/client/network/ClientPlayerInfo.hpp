// File: src/client/network/ClientPlayerInfo.hpp
//
// MC ClientPacketListener.playerInfoMap — every online player the server has
// listed (PlayerInfoS2C), the local one included: their name, game mode and
// colour. RemotePlayerManager holds the OTHER players' bodies; this is the
// list itself, which is what the tab list (PlayerTabOverlay) and the
// spectator menu's "teleport to player" page (SpectatorGui) read. Client main
// thread only (it is filled from the typed packet queue).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace Client {

    struct PlayerInfo {
        uint32_t    playerId = 0;
        std::string name;
        uint8_t     gameMode = 0;   // Server::GameMode raw value
        uint8_t     colorId  = 0;   // Game::PlayerColorId
        int32_t     latency  = 0;   // ms, MC PlayerInfo.getLatency (the ping bars)
        bool IsSpectator() const { return gameMode == 3; }
    };

    // Ordered by id — the join order, the stand-in for MC's profile-id order
    // (players have no UUIDs on this wire).
    inline std::map<uint32_t, PlayerInfo>& PlayerInfoMap() {
        static std::map<uint32_t, PlayerInfo> s_map;
        return s_map;
    }

    // The local player's id, as the connection learned it at login (0 before).
    inline uint32_t& LocalPlayerInfoId() {
        static uint32_t s_id = 0;
        return s_id;
    }

} // namespace Client
