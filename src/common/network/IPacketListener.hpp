// File: src/common/network/IPacketListener.hpp
#pragma once

#include <string>
#include <cstdint>

namespace Network {

    // Forward declarations for packet types
    struct ChunkDataS2CPacket;
    struct UnloadChunkS2CPacket;
    struct BlockChangeS2CPacket;
    struct MultiBlockChangeS2CPacket;
    struct PlayerUpdateS2CPacket;
    
    // C2S packet types
    class LoginStartC2SPacket;
    class HandshakeC2SPacket;
    class KeepAliveC2SPacket;

    // Base interface for all packet listeners
    // This follows Minecraft's visitor pattern for type-safe packet handling
    class IPacketListener {
    public:
        virtual ~IPacketListener() = default;
        
        // Get the name of this listener for debugging
        virtual const char* getName() const = 0;
        
        // ========================================================================
        // SERVER → CLIENT PACKET HANDLERS
        // ========================================================================
        
        // Chunk management
        virtual void onChunkDataS2C(const ChunkDataS2CPacket& packet) {}
        virtual void onUnloadChunkS2C(const UnloadChunkS2CPacket& packet) {}
        
        // Block updates
        virtual void onBlockChangeS2C(const BlockChangeS2CPacket& packet) {}
        virtual void onMultiBlockChangeS2C(const MultiBlockChangeS2CPacket& packet) {}
        
        // Player updates
        virtual void onPlayerUpdateS2C(const PlayerUpdateS2CPacket& packet) {}
        
        // Connection management
        virtual void onDisconnect(const std::string& reason) {}
        virtual void onKeepAlive(uint64_t id) {}
        
        // ========================================================================
        // CLIENT → SERVER PACKET HANDLERS
        // ========================================================================
        
        // Login phase
        virtual void onHandshake(const HandshakeC2SPacket& packet) {}
        virtual void onLoginStart(const LoginStartC2SPacket& packet) {}
        
        // Play phase
        virtual void onKeepAliveResponse(const KeepAliveC2SPacket& packet) {}
    };

} // namespace Network