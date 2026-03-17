// File: src/client/network/ClientConnection.hpp
#pragma once

#include "common/network/NetworkConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/S2CPackets.hpp"
#include "ClientPacketHandler.hpp"
#include <memory>
#include <string>

namespace Client {

    class NetworkClient;
    
    // Client-side connection handler
    class ClientConnection : public Network::NetworkConnection {
    public:
        // Constructor
        ClientConnection(tcp::socket socket, NetworkClient* client);
        ~ClientConnection() override;
        
        // ========================================================================
        // CONNECTION STATE
        // ========================================================================
        
        // Initiate handshake and login
        void StartHandshake(const std::string& playerName, const std::string& serverHost = "127.0.0.1", uint16_t serverPort = 0);
        
        // Check if logged in
        bool IsLoggedIn() const { return m_loggedIn; }
        
        // Get player name
        const std::string& GetPlayerName() const { return m_playerName; }
        
        // Get player ID
        uint32_t GetPlayerId() const { return m_playerId; }

        // ========================================================================
        // PACKET SENDING (CLIENT → SERVER)
        // ========================================================================
        
        // Send block action
        void SendBlockAction(const Network::BlockActionC2SPacket& packet);
        
        // Send player movement
        void SendPlayerMove(const Network::PlayerMoveC2SPacket& packet);
        
        // Send chat message
        void SendChatMessage(const std::string& message);
        
        // Send client settings
        void SendClientSettings(int renderDistance, bool vsync, float mouseSensitivity);
        
        // Send keep-alive response
        void SendKeepAliveResponse(uint64_t id);

        // ========================================================================
        // PACKET HANDLERS (OVERRIDE FROM BASE)
        // ========================================================================
        
        // Decode packet on I/O thread (creates typed packet)
        Network::PacketPtr DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        
        // Legacy callback (for backward compatibility)
        void OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        void OnConnected() override;
        void OnDisconnected() override;
        void OnError(const error_code& error) override;
        
        // Process incoming packets on main thread
        void DrainIncomingPackets();

    private:
        // ========================================================================
        // PACKET HANDLERS (SERVER → CLIENT)
        // ========================================================================
        
        // Handle login success
        void HandleLoginSuccess(const std::vector<uint8_t>& payload);
        
        // Handle disconnect
        void HandleDisconnect(const std::vector<uint8_t>& payload);
        
        // Handle block change
        void HandleBlockChange(const std::vector<uint8_t>& payload);
        
        // Handle chat message
        void HandleChatMessage(const std::vector<uint8_t>& payload);
        
        // Handle time update
        void HandleTimeUpdate(const std::vector<uint8_t>& payload);
        
        // Handle player abilities
        void HandlePlayerAbilities(const std::vector<uint8_t>& payload);
        
        // Handle world spawn
        void HandleWorldSpawn(const std::vector<uint8_t>& payload);

    private:
        // Client reference
        NetworkClient* m_client;
        
        // Player information
        std::string m_playerName;
        uint32_t m_playerId = 0;
        bool m_loggedIn = false;
        
        // Connection phase
        enum class ConnectionPhase {
            HANDSHAKING,
            LOGIN,
            PLAY
        };
        ConnectionPhase m_phase = ConnectionPhase::HANDSHAKING;
        
        // Packet registry for this connection
        Network::PacketRegistry m_packetRegistry;
        
        // World state
        glm::vec3 m_spawnPosition{0, 67, 0};
        uint64_t m_worldAge = 0;
        uint64_t m_timeOfDay = 6000; // Noon
        
        // Player state
        uint8_t m_playerAbilities = 0;
        float m_flySpeed = 0.05f;
        float m_walkSpeed = 0.1f;
    };
    
    using ClientConnectionPtr = std::shared_ptr<ClientConnection>;

} // namespace Client