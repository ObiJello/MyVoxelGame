// File: src/server/network/listeners/ServerPlayPacketListener.hpp
#pragma once

#include "common/network/IPacketListener.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/PacketTypes.hpp"

namespace Server {
    
    class ServerConnection;
    class PlayerSession;
    
    class ServerPlayPacketListener : public Network::IPacketListener {
    private:
        ServerConnection& m_connection;
        PlayerSession& m_session;  // Required session reference
        
    public:
        // Constructor requires session (no longer optional)
        ServerPlayPacketListener(ServerConnection& connection, PlayerSession& session);
        
        ~ServerPlayPacketListener() override = default;
        
        // Override from IPacketListener  
        void onKeepAliveResponse(const Network::KeepAliveC2SPacket& packet) override;
        const char* getName() const override { return "ServerPlayPacketListener"; }
        
        // Block interactions
        void handleUseItemOn(const Network::UseItemOnC2SPacket& packet);  // Minecraft-correct naming
        void onUseItemOnC2S(const Network::UseItemOnC2SPacket& packet) override {
            // Forward to handler method
            handleUseItemOn(packet);
        }
        void onBlockActionC2S(const Network::BlockActionC2SPacket& packet) override;

        // Item use in air (MC handleUseItem — ServerGamePacketListenerImpl.java:1329)
        void handleUseItem(const Network::UseItemC2SPacket& packet);
        void onUseItemC2S(const Network::UseItemC2SPacket& packet) override {
            handleUseItem(packet);
        }

        // Player actions (MC handlePlayerAction — ServerGamePacketListenerImpl.java:1191)
        void handlePlayerAction(const Network::PlayerActionC2SPacket& packet);
        void onPlayerActionC2S(const Network::PlayerActionC2SPacket& packet) override {
            handlePlayerAction(packet);
        }

        // Fly-state toggle (MC handlePlayerAbilities — only the FLYING bit is client-writable)
        void onPlayerAbilitiesC2S(const Network::PlayerAbilitiesC2SPacket& packet) override;
        void onInteractC2S(const Network::InteractC2SPacket& packet) override;
        
        // Player updates
        void onPlayerMoveC2S(const Network::PlayerMoveC2SPacket& packet) override;
        
        // Chat
        void onChatMessageC2S(const Network::ChatMessageC2SPacket& packet) override;

        // Held item change
        void onHeldItemChangeC2S(const Network::HeldItemChangeC2SPacket& packet) override;

        // Inventory click + close
        void onInventoryClickC2S(const Network::InventoryClickC2SPacket& packet) override;
        void onInventoryCloseC2S(const Network::InventoryCloseC2SPacket& packet) override;

        // Chunk batch acknowledgment
        void onChunkBatchAck(float desiredChunksPerTick) override;

        // Client reports its own level is ready (MC handleAcceptPlayerLoad)
        void onPlayerLoaded() override;
    };
    
} // namespace Server