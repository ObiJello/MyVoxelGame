// File: src/client/network/ClientConnection.hpp
#pragma once

#include <atomic>

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
        
        // Initiate handshake and login. `playerColor` is a Game::PlayerColorId
        // value cast to uint8_t — sent to the server so OTHER clients can render
        // this player's stick figure in the chosen colour.
        void StartHandshake(const std::string& playerName,
                            uint8_t playerColor = 0,
                            const std::string& serverHost = "127.0.0.1",
                            uint16_t serverPort = 0);
        
        // Check if logged in
        bool IsLoggedIn() const { return m_loggedIn; }

        // Bot swarm (src/client/dev/BotSwarm.hpp): in PLAY, decode only the
        // packets a headless bot acts on — position snaps, chunk batch
        // start/finish, dimension changes, chat, retained-chunk notices — and
        // turn everything else into a no-op packet. Chunk data is never
        // parsed or prebuilt, so a hundred bots in one process do not spend
        // the CPU a hundred real clients would. Set before connecting.
        void SetLightweightDecode(bool on) { m_lightweightDecode.store(on, std::memory_order_relaxed); }

        // See NetworkConnection::ShouldDeferPacket.
        bool ShouldDeferPacket(uint8_t packetId) const override;

        // See NetworkConnection::IsPacketAllowedOutbound. Ours is the
        // serverbound half of MC's per-phase protocol tables
        // (HandshakeProtocols / LoginProtocols / GameProtocols.SERVERBOUND_TEMPLATE).
        bool IsPacketAllowedOutbound(uint8_t packetId) const override;
        
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
        void SendClientSettings(int renderDistance, int simulationDistance,
                                bool vsync, float mouseSensitivity);
        
        // Send keep-alive response
        void SendKeepAliveResponse(uint64_t id);

        // F3+3 ping chart — MC PingDebugMonitor. The request carries the send
        // time; the server echoes it (PongResponseS2C) and the echo is timed
        // on the I/O thread, so the sample is a true socket round trip and
        // never includes a stalled main-thread frame.
        void SendPingRequest();
        // The most recent completed round trip in milliseconds, or -1 when no
        // pong has landed since the last call (the value is consumed).
        int32_t ConsumePingRtt();

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

        // ========================================================================
        // PACKET HANDLERS (SERVER → CLIENT)
        // ========================================================================
        //
        // Public, and called from ClientPacketHandler — which is where MC puts
        // the dispatch (ClientPacketListener) while reaching back through
        // `this.connection` for connection-owned state, exactly as these do for
        // the world age, spawn position and player id.
        //
        // CLIENT MAIN THREAD ONLY. Every one of these is reached from the typed
        // packet queue drained in DrainIncomingPackets. They used to run inline
        // on the network I/O thread, which raced the render thread reading the
        // very chunk cache they write.

        // BlockEntity create/update and remove (Stage 2 of the BE system).
        // Materialise (or destroy) a client-side BlockEntity in the
        // corresponding chunk's per-cell BE map so the dispatcher can render
        // it next frame.
        void HandleBlockEntityData(const Network::BlockEntityDataS2CPacket& packet);
        void HandleBlockEntityRemove(const Network::BlockEntityRemoveS2CPacket& packet);
        // MC ClientPacketListener.handleBlockEvent → level.blockEvent: the
        // client runs the block's triggerEvent itself (pistons, note blocks).
        void HandleBlockEvent(const Network::BlockEntityActionS2CPacket& packet);

        // Handle chat message
        void HandleChatMessage(const Network::ChatMessageS2CPacket& packet);

        // Handle time update
        void HandleTimeUpdate(const Network::TimeUpdateS2CPacket& packet);

        // Handle world spawn
        void HandleWorldSpawn(const Network::WorldSpawnS2CPacket& packet);

        // Handle player info update (join/leave with name) — MC: ClientboundPlayerInfoUpdatePacket
        void HandlePlayerInfo(const Network::PlayerInfoS2CPacket& packet);

        // Handle authoritative position snap from server (MC: ClientboundPlayerPositionPacket).
        // Calls the teleport callback registered by PlatformMain to snap the local Player,
        // then sends ServerboundAcceptTeleportation back with the same id.
        void HandleClientboundPlayerPosition(const Network::ClientboundPlayerPositionPacket& packet);

    private:
        // Login-phase handlers. These stay on the raw-payload path and run on
        // the network I/O thread, matching MC — ClientHandshakePacketListenerImpl
        // carries no ensureRunningOnSameThread call. For compression that is
        // mandatory, not stylistic: the decoder has to switch before the next
        // frame is read off the socket.
        void HandleLoginSuccess(const std::vector<uint8_t>& payload);
        void HandleDisconnect(const std::vector<uint8_t>& payload);


        // Client reference
        NetworkClient* m_client;
        
        // Player information
        std::string m_playerName;
        uint32_t m_playerId = 0;
        // Written by the PongResponseS2C handler on the I/O thread, read by
        // the debug overlay on the main thread (see ConsumePingRtt).
        std::atomic<int32_t> m_pingRttMs{-1};
        bool m_loggedIn = false;
        
        // Connection phase
        enum class ConnectionPhase {
            HANDSHAKING,
            LOGIN,
            PLAY
        };
        ConnectionPhase m_phase = ConnectionPhase::HANDSHAKING;
        std::atomic<bool> m_lightweightDecode{false};   // see SetLightweightDecode
        
        // Packet registry for this connection
        Network::PacketRegistry m_packetRegistry;
        
        // World state
        glm::vec3 m_spawnPosition{0, 67, 0};
        uint64_t m_worldAge = 0;
        uint64_t m_timeOfDay = 6000; // Noon
        bool m_doDaylightCycle = false;

    };
    
    using ClientConnectionPtr = std::shared_ptr<ClientConnection>;

} // namespace Client