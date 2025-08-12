// File: src/common/network/ProtocolTypes.hpp
#pragma once

#include <cstdint>

namespace Network {

    // Protocol states (mirrors Minecraft's connection states)
    enum class ProtocolState : uint8_t {
        HANDSHAKING = 0,
        STATUS = 1,
        LOGIN = 2,
        PLAY = 3
    };

    // Packet direction
    enum class PacketDirection : uint8_t {
        CLIENTBOUND = 0,  // Server -> Client
        SERVERBOUND = 1   // Client -> Server  
    };

} // namespace Network