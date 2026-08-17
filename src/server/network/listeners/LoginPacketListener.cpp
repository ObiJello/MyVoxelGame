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
            m_connection.SendPacket(static_cast<uint8_t>(Network::PacketId::SetCompression),
                                    buffer.GetData());

            m_connection.EnableCompression(m_compressionThreshold);

            Log::Info("[LoginPacketListener] Compression enabled, threshold %d bytes",
                      m_compressionThreshold);
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
        
        // Notify server that player has joined
        // This will create the PlayerSession and ServerPlayer
        if (m_server) {
            // Get shared_ptr from ServerConnection (which inherits enable_shared_from_this)
            auto connPtr = std::static_pointer_cast<ServerConnection>(m_connection.shared_from_this());
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
            m_connection.SendDisconnect("Server error: Failed to create player session");
            return;
        }
        
        // Switch to PLAY protocol state with session reference
        // This replaces the current listener, so it must be done after all operations
        // that might use this LoginPacketListener instance
        m_connection.setProtocolState(Network::ProtocolState::PLAY, session);
        
        Log::Info("[LoginPacketListener] Player %s successfully logged in and switched to PLAY state", 
                  username.c_str());
    }

    void LoginPacketListener::onDisconnect(const std::string& reason) {
        Log::Info("[LoginPacketListener] Connection closed during login: %s", reason.c_str());
    }

} // namespace Server