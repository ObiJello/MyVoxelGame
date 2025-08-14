// File: src/common/network/ProtocolTypes.hpp
#pragma once

#include <cstdint>

namespace Network {

    // Wire values for nextState field in HandshakeC2S packet
    // These match Minecraft protocol exactly
    enum class NextStateWire : int32_t {
        STATUS = 1,
        LOGIN = 2
    };

    // Internal connection phases (used for state machine)
    enum class ProtocolState : uint8_t {
        HANDSHAKING = 0,  // Initial phase, waiting for handshake
        STATUS = 1,       // Status phase (server list ping)
        LOGIN = 2,        // Login phase (authentication)
        PLAY = 3          // Play phase (in-game)
    };

    // Packet direction
    enum class PacketDirection : uint8_t {
        CLIENTBOUND = 0,  // Server -> Client
        SERVERBOUND = 1   // Client -> Server  
    };

} // namespace Network