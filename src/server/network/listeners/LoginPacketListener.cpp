// File: src/server/network/listeners/LoginPacketListener.cpp
#include "LoginPacketListener.hpp"
#include "../ServerConnection.hpp"
#include "../NetworkServer.hpp"
#include "../../IntegratedServer.hpp"
#include "../../session/PlayerSessionManager.hpp"  // For GetSession() call
#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

namespace Server {

    LoginPacketListener::LoginPacketListener(ServerConnection& connection, NetworkServer* server)
        : m_connection(connection)
        , m_server(server) {
    }

    void LoginPacketListener::onLoginStart(const Network::LoginStartC2SPacket& packet) {
        // DIAGNOSTIC: Log method entry
        Log::Info("[LoginPacketListener] *** onLoginStart() CALLED *** for player: %s (color id=%u)",
                  packet.username.c_str(), static_cast<unsigned>(packet.colorId));
        Log::Debug("[LoginPacketListener] Connection ID: %u", m_connection.GetConnectionId());

        // ── Compression (MC ServerLoginPacketListenerImpl.java:147) ─────────
        //
        //     if (server.getCompressionThreshold() >= 0 && !connection.isMemoryConnection()) {
        //        connection.send(new ClientboundLoginCompressionPacket(threshold),
        //           PacketSendListener.thenRun(() ->
        //              connection.setupCompression(threshold, true)));
        //     }
        //
        // Two details matter and both are reproduced below. The skip for a
        // memory connection — ours is the loopback test, since singleplayer
        // talks to the integrated server over 127.0.0.1 rather than an in-process
        // pipe — and the ORDER: SetCompression itself goes out uncompressed, and
        // the encoder only switches on afterwards. Enabling first would send the
        // client a compressed frame announcing compression it has not enabled.
        if (m_compressionThreshold >= 0 && !m_connection.IsLoopback()) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(m_compressionThreshold);

            // The enable runs in the SEND-COMPLETION hook, not inline. MC:
            //
            //   this.connection.send(new ClientboundLoginCompressionPacket(t),
            //      PacketSendListener.thenRun(() ->
            //         this.connection.setupCompression(t, true)));
            //
            // Enabling inline flips this connection's READER the instant the
            // call returns, while the announcing packet may still be sitting in
            // our own send queue. Anything the peer sends in the interim is
            // still old-framed, and inflating it yields Z_DATA_ERROR — which is
            // exactly how a remote client got kicked with
            // "Inflate failed (rc=-3, got 0 of 129)": 129 is PlayerMoveC2S's id
            // VarInt being read as a compressed frame's uncompressed-length.
            //
            // Capturing a shared_ptr keeps the connection alive until the hook
            // runs; the listener that owns this lambda may be long gone by then.
            auto conn = std::static_pointer_cast<ServerConnection>(m_connection.shared_from_this());
            const int threshold = m_compressionThreshold;
            m_connection.SendPacket(static_cast<uint8_t>(Network::PacketId::SetCompression),
                                    buffer.GetData(),
                                    [conn, threshold]() {
                                        conn->EnableCompression(threshold);
                                        Log::Info("[LoginPacketListener] Compression enabled "
                                                  "(post-write), threshold %d bytes", threshold);
                                    });
        } else if (m_connection.IsLoopback()) {
            Log::Info("[LoginPacketListener] Loopback peer — compression skipped "
                      "(MC does the same for its in-process connection)");
        }

        // Stash the player's chosen colour on the connection BEFORE finalizeLogin
        // calls IntegratedServer::OnPlayerJoined — that method reads
        // connection->GetPlayerColor() to populate the new ServerPlayer's colour
        // and to populate the PlayerInfoS2C ADD broadcasts other clients receive.
        m_connection.SetPlayerColor(packet.colorId);

        // For offline/integrated mode, skip encryption and finalize login directly
        finalizeLogin(packet.username);
    }

    void LoginPacketListener::finalizeLogin(const std::string& username) {
        Log::Debug("[LoginPacketListener] finalizeLogin() called for %s", username.c_str());
        // Generate player ID (use connection ID for now)
        uint32_t playerId = m_connection.GetConnectionId();
        
        // Send LoginSuccess packet
        Network::PacketBuffer buffer;
        buffer.WriteString(std::to_string(playerId));  // UUID as string
        buffer.WriteString(username);
        
        Log::Info("[LoginPacketListener] Sending LoginSuccess for player %s (ID: %u)", 
                  username.c_str(), playerId);
        
        m_connection.SendPacket(static_cast<uint8_t>(Network::PacketId::LoginSuccess), buffer.GetData());
        
        // Mark as authenticated BEFORE switching protocol state
        m_connection.setAuthenticated(true, playerId, username);
        
        // Send initial game data
        m_connection.sendInitialGameData();
        
        // ── EVERYTHING BELOW THIS LINE MUST NOT TOUCH `this` ───────────────
        //
        // OnPlayerJoined switches the connection to PLAY, which replaces
        // ServerConnection::m_listener — the unique_ptr that owns THIS object.
        // The instance survives the call only because ServerConnection parks
        // displaced listeners until the dispatch returns (see RetireListener);
        // even so, treating `this` as live past here is asking for the
        // use-after-free that reading m_connection here caused.
        //
        // So bind the connection to a local first. It is a separate object with
        // its own lifetime and stays valid regardless.
        ServerConnection& connection = m_connection;

        // Notify server that player has joined
        // This will create the PlayerSession and ServerPlayer
        if (m_server) {
            // Get shared_ptr from ServerConnection (which inherits enable_shared_from_this)
            auto connPtr = std::static_pointer_cast<ServerConnection>(connection.shared_from_this());
            m_server->OnPlayerJoined(connPtr);
        }

        // Get the PlayerSession that was just created by PlayerSessionManager
        PlayerSession* session = nullptr;
        if (Server::g_integratedServer) {
            auto sessionManager = Server::g_integratedServer->GetSessionManager();
            if (sessionManager) {
                auto sessionPtr = sessionManager->GetSession(playerId);
                session = sessionPtr.get();
                Log::Debug("[LoginPacketListener] Retrieved session %u from PlayerSessionManager",
                           playerId);
            } else {
                Log::Error("[LoginPacketListener] SessionManager not available");
            }
        }
        
        // Session is REQUIRED for PLAY state
        if (!session) {
            Log::Error("[LoginPacketListener] Failed to get PlayerSession for player %s", username.c_str());
            connection.SendDisconnect("Server error: Failed to create player session");
            return;
        }

        // The switch to PLAY is NOT done here any more. IntegratedServer::
        // OnPlayerJoined performs it the moment the session is wired, before it
        // sends any join packet — mirroring MC PlayerList.placeNewPlayer, where
        // setupInboundProtocol (:154) precedes every send and the teleport
        // (:179). Doing it here meant the join teleport went out while the
        // connection still read as LOGIN, and the client's ack raced us.
        //
        // Verify rather than assume: if OnPlayerJoined bailed before the
        // switch, the connection would sit in LOGIN with a live session and
        // silently ignore everything the client sends.
        if (connection.getPhase() != ServerConnection::ConnectionPhase::PLAY) {
            Log::Error("[LoginPacketListener] Player %s has a session but the connection "
                       "never reached PLAY — OnPlayerJoined did not complete",
                       username.c_str());
            connection.SendDisconnect("Server error: Failed to enter play state");
            return;
        }

        Log::Info("[LoginPacketListener] Player %s successfully logged in and switched to PLAY state",
                  username.c_str());
    }

    void LoginPacketListener::onDisconnect(const std::string& reason) {
        Log::Info("[LoginPacketListener] Connection closed during login: %s", reason.c_str());
    }

} // namespace Server