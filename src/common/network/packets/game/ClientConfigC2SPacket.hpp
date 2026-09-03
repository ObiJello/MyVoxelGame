// File: src/common/network/packets/game/ClientConfigC2SPacket.hpp
//
// Client-side settings the server cares about (render distance for chunk
// streaming, etc.). Closest MC equivalent is
// ServerboundClientInformationPacket from the configuration phase.
//
// NOTE: this packet has no Serialize/Deserialize implementation yet — the
// existing in-engine path uses it as a struct only. Wire it up here when
// the network layer starts emitting it.
#pragma once

#include "common/network/PacketRegistry.hpp"
#include <cstdint>
#include <vector>

namespace Network {

    // Wire order: VarInt renderDistance, byte vsync, float mouseSensitivity,
    // VarInt simulationDistance (appended last; see ClientConnection::
    // SendClientSettings / ServerConnection::DecodePacket).
    struct ClientConfigC2SPacket {
        int   renderDistance     = 8;
        bool  enableVSync        = true;
        float mouseSensitivity   = 1.0f;
        // MC ClientInformation carries only viewDistance; the simulation
        // distance is a server property there. Ours is per-client because
        // the integrated server is this player's machine and the option
        // lives in their options.txt — the same place MC's integrated server
        // reads it from (IntegratedServer.tickServer:119-131).
        int   simulationDistance = 12;
    };

} // namespace Network
