// File: src/server/network/ServerConnection.hpp
#pragma once

#include "common/network/NetworkConnection.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/ProtocolTypes.hpp"
#include "common/network/IPacketListener.hpp"
#include "common/network/packets/game/ChatMessageS2CPacket.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <atomic>

namespace Server {

    class NetworkServer;
    class ServerPlayer;

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
        uint8_t GetPlayerColor() const { return m_playerColor; }
        void    SetPlayerColor(uint8_t id) { m_playerColor = id; }
        
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
        // Styled form — colours, click-to-copy, hover text. The plain overload
        // above wraps a single unstyled run, so existing callers are unchanged.
        void SendChatMessage(const Network::ChatMessageS2CPacket& packet);
        
        // Send keep-alive
        void SendKeepAlive(uint64_t id);
        
        // Handle keep-alive response (public for listener)
        void HandleKeepAliveResponse(const std::vector<uint8_t>& payload);
        
        // Send disconnect
        void SendDisconnect(const std::string& reason);
        
        // Send time update
        void SendTimeUpdate(uint64_t worldAge, uint64_t timeOfDay, bool doDaylightCycle);
        void SendCurrentTimeUpdate(); // reads live values from the server world
        
        // Send player abilities + game mode built from the live ServerPlayer
        // (MC ClientboundPlayerAbilitiesPacket + CHANGE_GAME_MODE folded).
        void SendPlayerAbilities(const ServerPlayer& player);
        // Login-time abilities, built from the world's game mode. The
        // ServerPlayer doesn't exist yet at this point, but the mode does —
        // see the definition for why this must NOT be a survival placeholder.
        void SendPlayerAbilitiesForJoin();

        // Authoritative teleport (matches MC's ServerGamePacketListenerImpl.teleport overload).
        // Increments awaiting-teleport id, snaps the player's ServerPlayer position, and sends
        // ClientboundPlayerPosition to the client. The client must echo the id back via
        // ServerboundAcceptTeleportation so the server can ignore stale C2S position packets.
        //
        // Velocity overload — used by portal teleports to carry the player's
        // momentum through the destination, rotated by the portal-pair
        // matrix (server side). Velocity is shipped in the packet's
        // dx/dy/dz fields (MC's "deltaMovement"). Default zero matches MC's
        // standard teleport behaviour (kills velocity on snap).
        void Teleport(double x, double y, double z, float yRot, float xRot,
                      double dx, double dy, double dz);
        void Teleport(double x, double y, double z, float yRot, float xRot) {
            Teleport(x, y, z, yRot, xRot, 0.0, 0.0, 0.0);
        }

        // Port of MC ServerGamePacketListenerImpl.updateAwaitingTeleport
        // (:1148). Returns true while a server-initiated teleport is
        // outstanding — PlayerSession uses that to ignore the POSITION in C2S
        // move packets that were already in flight at the old location, which
        // would otherwise snap the ServerPlayer back and make other clients see
        // the teleported player flicker.
        //
        // The important half is the retry. If the ack has not arrived within 20
        // ticks it RE-SENDS the teleport, forever, so a dropped or discarded
        // ack costs one second rather than wedging the player for the rest of
        // the session. That is not a nicety: the old one-shot bool version is
        // exactly how a single lost ack froze movement on Windows, which then
        // presented as "I can't hit mobs" once the player walked out of reach
        // of their own frozen server-side position.
        //
        // Must be called from the server thread (it can send a packet).
        bool UpdateAwaitingTeleport();

        // Clear the gate for a client-echoed teleport id (MC
        // ServerGamePacketListenerImpl.handleAcceptTeleportPacket). Called from
        // ServerPlayPacketListener, i.e. on the SERVER thread — see the comment
        // on the decode case in ServerConnection::DecodePacket for why this
        // packet must not be handled inline on the network I/O thread.
        void AcceptTeleportation(int32_t teleportId);

        // Chat + command dispatch. Lives on the connection rather than the
        // listener because it reads m_playerName / m_server / *this to format
        // and broadcast; the listener calls it. Server thread only.
        void HandleChatMessage(const Network::ChatMessageC2SPacket& packet);

        // Apply client settings (render distance is the one that matters).
        // Reached from two places on purpose, mirroring MC: the typed PLAY-phase
        // path via the listener, and the pre-PLAY inline path where there is no
        // session yet and the value is parked for one.
        void ApplyClientSettings(int renderDistance, bool vsync, float mouseSensitivity);

        // ========================================================================
        // PACKET HANDLERS (OVERRIDE FROM BASE)
        // ========================================================================
        
        Network::PacketPtr DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        void OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) override;
        bool ShouldDeferPacket(uint8_t packetId) const override;
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
        
        // Pre-PLAY client settings, still on the raw-payload path — see
        // ApplyClientSettings. Every other C2S packet now has a typed
        // representation and reaches the listener on the server thread.
        void HandleClientSettings(const std::vector<uint8_t>& payload);

        // ========================================================================
        // INTERNAL HELPERS
        // ========================================================================
        
        // Park the outgoing listener so it outlives the handler that is
        // (usually) still running inside it. See m_retiredListeners.
        void RetireListener() {
            if (m_listener) m_retiredListeners.push_back(std::move(m_listener));
        }

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
        // Stick-figure colour from the client's LoginStart (Game::PlayerColorId
        // value). Echoed in PlayerInfoS2C ADD broadcasts so other clients render
        // this player in the right colour. 0 = Default neon green.
        uint8_t m_playerColor = 0;

        // Teleport tracking — matches MC's awaitingTeleport. Incremented per Teleport() call;
        // client must echo this id back so we can ignore stale C2S position packets that
        // were in flight before the teleport.
        int32_t m_awaitingTeleport = 0;
        // MC ServerGamePacketListenerImpl.awaitingPositionFromClient: the
        // position we teleported the player to, held until the client echoes
        // the id back. Engaged = a teleport is outstanding. A nullable POSITION
        // rather than a bare bool because the retry below has to re-send it.
        std::optional<glm::dvec3> m_awaitingPositionFromClient;
        // MC awaitingTeleportTime — the tick the current teleport was issued,
        // for the 20-tick retry. Paired with m_tickCount, bumped in tick().
        int32_t m_awaitingTeleportTime = 0;
        int32_t m_tickCount = 0;
        
        // Connection state
        // Atomic because it is genuinely cross-thread now: written on the
        // SERVER thread (setProtocolState, from OnPlayerJoined) and read on the
        // NETWORK I/O thread by DecodePacket and — critically —
        // ShouldDeferPacket, which uses it to decide whether a packet may run
        // inline. A stale read there would put a packet on the wrong thread,
        // which is the exact failure this whole rework exists to prevent.
        // Relaxed ordering is enough: nothing is published through this flag,
        // and the packet queue provides the ordering that matters.
        std::atomic<ConnectionPhase> m_phase{ConnectionPhase::HANDSHAKING};
        
        // Current packet listener (based on protocol state)
        std::unique_ptr<Network::IPacketListener> m_listener;

        // Listeners displaced by setProtocolState, kept alive until the current
        // packet dispatch returns.
        //
        // A protocol switch usually happens FROM INSIDE a handler on the
        // listener being replaced — LoginPacketListener::finalizeLogin is the
        // canonical case. Assigning straight over m_listener destroys that
        // object while its own method is still on the stack, so every member
        // access after the call is a use-after-free. MC gets away with the same
        // pattern (Connection.setupInboundProtocol reassigns this.packetListener
        // mid-handler) only because the GC keeps the old listener alive until
        // the stack unwinds. This is that guarantee, done by hand.
        //
        // A vector, not a single slot: two switches within one dispatch must
        // not let the second free the listener the first is still running in.
        // Cleared at the top of each tick() drain iteration, by which point the
        // previous handler has returned.
        std::vector<std::unique_ptr<Network::IPacketListener>> m_retiredListeners;
        
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