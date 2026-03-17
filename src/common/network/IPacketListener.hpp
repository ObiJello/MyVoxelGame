// File: src/common/network/IPacketListener.hpp
#pragma once

#include <string>
#include <cstdint>

namespace Network {

    // Forward declarations for packet types
    struct ChunkDataS2CPacket;
    struct UnloadChunkS2CPacket;
    struct BlockChangeS2CPacket;
    struct ClientboundBlockUpdateS2CPacket;
    struct ClientboundSectionBlocksUpdateS2CPacket;
    struct MultiBlockChangeS2CPacket;
    struct PlayerUpdateS2CPacket;
    struct RemoveEntitiesS2CPacket;
    struct HotbarSyncS2CPacket;
    
    // C2S packet types
    struct UseItemOnC2SPacket;
    struct BlockActionC2SPacket;
    struct PlayerMoveC2SPacket;
    struct ChatMessageC2SPacket;
    struct HeldItemChangeC2SPacket;
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
        virtual void onChunkBatchStart() {}
        virtual void onChunkBatchFinished(int batchSize) {}
        
        // Block updates
        virtual void onBlockChangeS2C(const BlockChangeS2CPacket& packet) {}
        virtual void onClientboundBlockUpdate(const ClientboundBlockUpdateS2CPacket& packet) {}
        virtual void onClientboundSectionBlocksUpdate(const ClientboundSectionBlocksUpdateS2CPacket& packet) {}
        virtual void onMultiBlockChangeS2C(const MultiBlockChangeS2CPacket& packet) {}
        
        // Player updates
        virtual void onPlayerUpdateS2C(const PlayerUpdateS2CPacket& packet) {}

        // Entity removal (Minecraft's ClientboundRemoveEntitiesPacket)
        virtual void onRemoveEntitiesS2C(const RemoveEntitiesS2CPacket& packet) {}

        // Inventory sync
        virtual void onHotbarSyncS2C(const HotbarSyncS2CPacket& packet) {}

        // View distance
        virtual void onSetChunkCacheRadiusS2C(int viewDistance) {}
        
        // Connection management
        virtual void onDisconnect(const std::string& reason) {}
        virtual void onKeepAlive(uint64_t id) {}
        
        // ========================================================================
        // CLIENT → SERVER PACKET HANDLERS
        // ========================================================================
        
        // Login phase
        virtual void onHandshake(const HandshakeC2SPacket& packet) {}
        virtual void onLoginStart(const LoginStartC2SPacket& packet) {}
        
        // Play phase - Block interactions
        virtual void onUseItemOnC2S(const UseItemOnC2SPacket& packet) {}
        virtual void onBlockActionC2S(const BlockActionC2SPacket& packet) {}
        
        // Play phase - Player updates
        virtual void onPlayerMoveC2S(const PlayerMoveC2SPacket& packet) {}
        
        // Play phase - Chat
        virtual void onChatMessageC2S(const ChatMessageC2SPacket& packet) {}

        // Play phase - Held item change
        virtual void onHeldItemChangeC2S(const HeldItemChangeC2SPacket& packet) {}
        
        // Play phase - Keep alive
        virtual void onKeepAliveResponse(const KeepAliveC2SPacket& packet) {}

        // Play phase - Chunk batch acknowledgment
        virtual void onChunkBatchAck(float desiredChunksPerTick) {}
    };

} // namespace Network