// File: src/server/player/PlayerAppearances.hpp
//
// The server's half of player appearance (docs/player-appearance.md): every
// connected player's look — stick figure, painted stick figure, or skin with
// its cape — as that player's client sent it (PlayerAppearanceC2S), validated
// and relayed to everyone else (PlayerAppearanceS2C), and every present
// player's sent to a newcomer.
//
// MC's equivalent is the profile's "textures" property riding
// ClientboundPlayerInfoUpdatePacket. The look is not saved with the player:
// each session's client sends it again on login (the launcher is where it is
// chosen).
//
// Server tick thread only (the packet listeners and the join / leave paths).
#pragma once

#include <cstdint>

namespace Network { struct PlayerAppearanceC2SPacket; }

namespace Server {

    class PlayerSession;
    class ServerConnection;

    namespace PlayerAppearances {

        // A client's own look arrived: validate it, keep it on the player's
        // id, and relay it to every other connected client.
        void HandleC2S(PlayerSession& session, const Network::PlayerAppearanceC2SPacket& packet);

        // A player joined: every other present player's look to them.
        // (Their own reaches everyone when their client sends it.)
        void SyncOnJoin(ServerConnection& joining, uint32_t joinerId);

        // A player left: forget their look — ids are reused.
        void Forget(uint32_t playerId);

        // Everything forgotten (server shutdown).
        void Clear();

    } // namespace PlayerAppearances

} // namespace Server
