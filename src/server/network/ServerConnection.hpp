// File: src/server/network/ServerConnection.hpp
#pragma once

#include "common/network/NetworkConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/ProtocolTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include <memory>
#include <string>
#include <atomic>

namespace Server {

    class NetworkServer;
    
    // Server-side connection handler for a single client
    // Mirrors Minecraft's ServerPlayNetworkHandler
    class ServerConnection : public Network::NetworkConnection {
    public:
        // Connection state/phase
        enum class ConnectionPhase {
            HANDSHAKING,
            STATUS,
            LOGIN,
            PLAY
        };
        
        // Constructor
        ServerConnection(tcp::socket socket, NetworkServer* server);
        ~ServerConnection() override;
        
        // ========================================================================
        // CONNECTION INFO
        // ========================================================================
        
        // Get/set player name
        void SetPlayerName(const std::string& name) { m_playerName = name; }
        const std::string& GetPlayerName() const { return m_playerName; }
        
        // Get/set player ID
        void SetPlayerId(uint32_t id) { m_playerId = id; }
        uint32_t GetPlayerId() const { return m_playerId; }
        
        // Get connection ID (unique per connection)
        uint32_t GetConnectionId() const { return m_connectionId; }
        
        // Check if authenticated
        bool IsAuthenticated() const { return m_authenticated; }
        void setAuthenticated(bool auth, uint32_t playerId, const std::string& name) {
            m_authenticated = auth;
            m_playerId = playerId;
            m_playerName = name;
        }
        
        // Get server reference
        NetworkServer* GetServer() const { return m_server; }
        
        // Get current protocol phase
        ConnectionPhase getPhase() const { return m_phase; }
        
        // Tick connection (drain packets on server thread)
        void tick();
        
        // Set protocol state and swap listener
        void setProtocolState(Network::ProtocolState state);
        
        // Set protocol state with PlayerSession (for PLAY state)
        void setProtocolState(Network::ProtocolState state, class PlayerSession* session);
        
        // Send initial game data after login
        void sendInitialGameData();

        // ========================================================================
        // PACKET SENDING (SERVER → CLIENT)
        // ========================================================================
        
        // Send block change
        void SendBlockChange(const Network::BlockChangeS2CPacket& packet);
        
        // Send chat message
        void SendChatMessage(const std::string& message, uint8_t position = 0, uint32_t senderId = 0);
        
        // Send keep-alive
        void SendKeepAlive(uint64_t id);
        
        // Handle keep-alive response (public for listener)
        void HandleKeepAliveResponse(const std::vector<uint8_t>& payload);
        
        // Send disconnect
        void SendDisconnect(const std::string& reason);
        
        // Send time update
        void SendTimeUpdate(uint64_t worldAge, uint64_t timeOfDay);
        
        // Send player abilities
        void SendPlayerAbilities(uint8_t flags, float flySpeed, float walkSpeed);

        // Authoritative teleport (matches MC's ServerGamePacketListenerImpl.teleport overload).
        // Increments awaiting-teleport id, snaps the player's ServerPlayer position, and sends
        // ClientboundPlayerPosition to the client. The client must echo the id back via
        // ServerboundAcceptTeleportation so the server can ignore stale C2S position packets.
        void Teleport(double x, double y, double z, float yRot, float xRot);

        // ========================================================================
        // PACKET HANDLERS (OVERRIDE FROM BASE)
        // ========================================================================
        
        Network::PacketPtr DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        void OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        void OnConnected() override;
        void OnDisconnected() override;
        void OnError(const error_code& error) override;

    private:
        // Connection ID (assigned on creation)
        static std::atomic<uint32_t> s_nextConnectionId;
        uint32_t m_connectionId;
        
        // ========================================================================
        // PACKET HANDLERS (CLIENT → SERVER)
        // ========================================================================
        
        // Handle handshake
        void HandleHandshake(const std::vector<uint8_t>& payload);
        
        // Handle login start
        void HandleLoginStart(const std::vector<uint8_t>& payload);
        
        // Handle block action
        void HandleBlockAction(const std::vector<uint8_t>& payload);
        
        // Handle player move
        void HandlePlayerMove(const std::vector<uint8_t>& payload);
        
        // Handle chat message
        void HandleChatMessage(const std::vector<uint8_t>& payload);
        
        // Handle client settings
        void HandleClientSettings(const std::vector<uint8_t>& payload);

        // Handle held item change
        void HandleHeldItemChange(const std::vector<uint8_t>& payload);

        // Handle inventory click and close
        void HandleInventoryClick(const std::vector<uint8_t>& payload);
        void HandleInventoryClose(const std::vector<uint8_t>& payload);

        // Handle ack for a previously sent ClientboundPlayerPosition (MC: handleAcceptTeleportPacket).
        // Validates the id matches m_awaitingTeleport — stale acks are ignored.
        void HandleAcceptTeleportation(const std::vector<uint8_t>& payload);

        // ========================================================================
        // INTERNAL HELPERS
        // ========================================================================
        
        // Validate packet size
        bool ValidatePacketSize(const std::vector<uint8_t>& payload, size_t expectedMin);
        
        // Check rate limits
        bool CheckRateLimit(const std::string& action);
        
        // Update last activity time
        void UpdateActivity() { m_lastActivity = std::chrono::steady_clock::now(); }
        
        // Check for timeout
        bool IsTimedOut() const;

    private:
        // Server reference
        NetworkServer* m_server;
        
        // Player information
        std::string m_playerName;
        uint32_t m_playerId = 0;
        bool m_authenticated = false;

        // Teleport tracking — matches MC's awaitingTeleport. Incremented per Teleport() call;
        // client must echo this id back so we can ignore stale C2S position packets that
        // were in flight before the teleport.
        int32_t m_awaitingTeleport = 0;
        
        // Connection state
        ConnectionPhase m_phase = ConnectionPhase::HANDSHAKING;
        
        // Current packet listener (based on protocol state)
        std::unique_ptr<Network::IPacketListener> m_listener;
        
        // Keep-alive tracking
        uint64_t m_lastKeepAliveId = 0;
        uint64_t m_keepAliveSequence = 0;
        bool m_awaitingKeepAlive = false;
        std::chrono::steady_clock::time_point m_lastKeepAliveSent;
        std::chrono::steady_clock::time_point m_lastKeepAliveReceived;
        std::chrono::steady_clock::time_point m_lastPacketReceived;
        
        // Activity tracking
        std::chrono::steady_clock::time_point m_lastActivity;
        
        // Rate limiting
        struct RateLimit {
            size_t count = 0;
            std::chrono::steady_clock::time_point resetTime;
        };
        std::unordered_map<std::string, RateLimit> m_rateLimits;
        
        // Packet registry for this connection
        Network::PacketRegistry m_packetRegistry;
        
        // Timeout settings
        static constexpr auto KEEP_ALIVE_INTERVAL = std::chrono::seconds(15);
        static constexpr auto CONNECTION_TIMEOUT = std::chrono::seconds(30);
        static constexpr auto LOGIN_TIMEOUT = std::chrono::seconds(10);
    };
    
    using ServerConnectionPtr = std::shared_ptr<ServerConnection>;

} // namespace Server