// File: src/server/network/ServerConnection.cpp
#include "ServerConnection.hpp"
#include "NetworkServer.hpp"
#include "../commands/CommandDispatcher.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "listeners/HandshakePacketListener.hpp"
#include "listeners/LoginPacketListener.hpp"
#include "listeners/ServerPlayPacketListener.hpp"
#include "../session/PlayerSession.hpp"
#include "../player/ServerPlayer.hpp"
#include "../IntegratedServer.hpp"
#include "common/core/Log.hpp"
#include <limits>
#include "common/network/packets/HandshakeC2S.hpp"
#include "common/network/packets/LoginStartC2S.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
#include "common/network/packets/C2SPackets.hpp"
#include "common/network/PacketRegistry.hpp"
#include "../IntegratedServer.hpp"

namespace Server {

    // Static connection ID counter
    std::atomic<uint32_t> ServerConnection::s_nextConnectionId{1};

    ServerConnection::ServerConnection(tcp::socket socket, NetworkServer* server)
        : NetworkConnection(std::move(socket))
        , m_server(server)
        , m_connectionId(s_nextConnectionId.fetch_add(1))
    {
        // Set connection name to "Server#id" for clearer logging
        SetName("Server#" + std::to_string(m_connectionId));
        
        m_lastActivity = std::chrono::steady_clock::now();
        m_lastKeepAliveSent = m_lastActivity;
        m_lastKeepAliveReceived = m_lastActivity;
        m_lastPacketReceived = m_lastActivity;
        
        // Start with handshake listener
        setProtocolState(Network::ProtocolState::HANDSHAKING);
        
        // Register packet handlers (legacy - will be replaced by listener system)
        using namespace Network;
        // DISABLED: These conflict with the new listener system
        // m_packetRegistry.RegisterHandler(PacketId::Handshake,
        //                                  [this](const std::vector<uint8_t>& p) { HandleHandshake(p); });
        // m_packetRegistry.RegisterHandler(PacketId::LoginStart,
        //                                  [this](const std::vector<uint8_t>& p) { HandleLoginStart(p); });
        m_packetRegistry.RegisterHandler(PacketId::BlockActionC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleBlockAction(p); });
        m_packetRegistry.RegisterHandler(PacketId::PlayerMoveC2S,
                                         [this](const std::vector<uint8_t>& p) { HandlePlayerMove(p); });
        m_packetRegistry.RegisterHandler(PacketId::ChatMessageC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleChatMessage(p); });
        m_packetRegistry.RegisterHandler(PacketId::KeepAliveC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleKeepAliveResponse(p); });
        m_packetRegistry.RegisterHandler(PacketId::ClientConfigC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleClientSettings(p); });
        m_packetRegistry.RegisterHandler(PacketId::HeldItemChange,
                                         [this](const std::vector<uint8_t>& p) { HandleHeldItemChange(p); });
        m_packetRegistry.RegisterHandler(PacketId::ServerboundAcceptTeleportation,
                                         [this](const std::vector<uint8_t>& p) { HandleAcceptTeleportation(p); });
        m_packetRegistry.RegisterHandler(PacketId::InventoryClickC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleInventoryClick(p); });
        m_packetRegistry.RegisterHandler(PacketId::InventoryCloseC2S,
                                         [this](const std::vector<uint8_t>& p) { HandleInventoryClose(p); });
    }

    ServerConnection::~ServerConnection() {
        // Don't call shared_from_this() in destructor as it can throw
        // Connection cleanup is handled by OnDisconnected() callback
    }

    void ServerConnection::OnConnected() {
        Log::Info("[ServerConnection %u] Connected from %s", 
            GetConnectionId(), GetRemoteEndpoint().address().to_string().c_str());
        
        if (m_server) {
            m_server->OnConnectionEstablished(
                std::static_pointer_cast<ServerConnection>(shared_from_this()));
        }
    }

    void ServerConnection::OnDisconnected() {
        Log::Info("[ServerConnection %u] Disconnected (%s)", 
            GetConnectionId(), m_playerName.empty() ? "unnamed" : m_playerName.c_str());
        
        if (m_server) {
            try {
                // Safely get shared_ptr - this can throw if we're being destroyed
                auto self = shared_from_this();
                m_server->OnConnectionClosed(
                    std::static_pointer_cast<ServerConnection>(self));
            } catch (const std::bad_weak_ptr& e) {
                // Object is being destroyed, can't get shared_ptr
                Log::Debug("[ServerConnection %u] Already being destroyed, skipping server notification", 
                    GetConnectionId());
            } catch (const std::exception& e) {
                // Other errors during notification
                Log::Warning("[ServerConnection %u] Failed to notify server of disconnection: %s", 
                    GetConnectionId(), e.what());
            }
        }
    }

    void ServerConnection::OnError(const error_code& error) {
        Log::Error("[ServerConnection %u] Error: %s", 
            GetConnectionId(), error.message().c_str());
    }
    
    void ServerConnection::tick() {
        // Drain incoming packets queue and apply to listener
        int packetsProcessed = 0;
        const int MAX_PACKETS_PER_TICK = 1000;  // Safety limit
        
        // State-aware budgeting (Minecraft-style)
        float budgetMs;
        switch (m_phase) {
            case ConnectionPhase::HANDSHAKING:
            case ConnectionPhase::LOGIN:
            case ConnectionPhase::STATUS:
                budgetMs = 5.0f;  // Generous 5ms for connection setup
                break;
            case ConnectionPhase::PLAY:
                budgetMs = 1.0f;  // 1ms for normal play (increased from 0.5ms for older systems)
                break;
            default:
                budgetMs = 1.0f;
                break;
        }
        
        auto startTime = std::chrono::steady_clock::now();
        
        // Peek-then-pop pattern (Minecraft-style: never lose packets)
        while (packetsProcessed < MAX_PACKETS_PER_TICK) {
            // Check if queue is empty
            if (!HasIncomingPackets()) {
                break;
            }
            
            // Check time budget (but always process at least 1 packet to prevent starvation)
            if (packetsProcessed > 0) {  // Already processed at least one
                auto currentTime = std::chrono::steady_clock::now();
                float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
                
                // Peek at the next packet to check if it's critical
                bool isCritical = false;
                PeekIncoming([&isCritical](const Network::IncomingPacket& pkt) {
                    if (pkt.packet) {
                        auto packetId = pkt.packet->getId();
                        isCritical = (packetId == Network::PacketId::KeepAliveC2S ||
                                     packetId == Network::PacketId::Disconnect ||
                                     packetId == Network::PacketId::Handshake ||
                                     packetId == Network::PacketId::LoginStart);
                    }
                });
                
                // Stop if over budget and not a critical packet
                if (elapsedMs >= budgetMs && !isCritical) {
                    Log::Debug("[ServerConnection %u] Time budget of %.1fms exceeded after %.2fms, leaving %zu packets for next tick", 
                              GetConnectionId(), budgetMs, elapsedMs, GetIncomingQueueSize());
                    break;
                }
            }
            
            // NOW we're committed to processing this packet, so pop it
            Network::IncomingPacket packet;
            if (!TryPopIncoming(packet)) {
                // Shouldn't happen since we checked HasIncomingPackets, but be safe
                Log::Warning("[ServerConnection %u] Failed to pop packet from non-empty queue", GetConnectionId());
                break;
            }
            
            // Process the packet
            if (packet.packet) {
                // Check if we need to create a listener based on the packet type
                if (!m_listener) {
                    if (packet.packet->getId() == Network::PacketId::Handshake) {
                        m_listener = std::make_unique<HandshakePacketListener>(*this);
                    } else if (packet.packet->getId() == Network::PacketId::LoginStart && 
                               m_phase == ConnectionPhase::LOGIN) {
                        m_listener = std::make_unique<LoginPacketListener>(*this, m_server);
                    }
                }
                
                if (m_listener) {
                    try {
                        // Apply packet to current listener (visitor pattern)
                        if (auto* c2sPacket = dynamic_cast<Network::IC2SPacket*>(packet.packet.get())) {
                            // Store listener name before apply (it might change during apply)
                            std::string listenerName = m_listener->getName();
                            
                            // Apply the packet - this might change m_listener!
                            c2sPacket->apply(*m_listener);
                            packetsProcessed++;
                            
                            // Log if listener changed (for debugging)
                            if (m_listener && m_listener->getName() != listenerName) {
                                Log::Debug("[ServerConnection %u] Listener changed from %s to %s after packet 0x%02X",
                                          GetConnectionId(), listenerName.c_str(), m_listener->getName(),
                                          static_cast<int>(packet.packet->getId()));
                            }
                        }
                    } catch (const std::exception& e) {
                        Log::Error("[ServerConnection %u] Exception processing packet: %s", 
                                  GetConnectionId(), e.what());
                    }
                } else {
                    Log::Warning("[ServerConnection %u] No listener set for packet ID 0x%02X in phase %d", 
                                GetConnectionId(), static_cast<int>(packet.packet->getId()), 
                                static_cast<int>(m_phase));
                }
            }
        }
        
        // Log only if we processed packets or have a backlog
        if (packetsProcessed > 0 || GetIncomingQueueSize() > 10) {
            Log::Debug("[ServerConnection %u] Processed %d packets, %zu remaining in queue", 
                      GetConnectionId(), packetsProcessed, GetIncomingQueueSize());
        }
        
        // Send periodic keep-alives during PLAY phase
        if (m_phase == ConnectionPhase::PLAY) {
            auto now = std::chrono::steady_clock::now();
            
            if (!m_awaitingKeepAlive) {
                // Send new keep-alive if interval has passed since last keep-alive sent
                if (now - m_lastKeepAliveSent >= KEEP_ALIVE_INTERVAL) {
                    // Generate a new keep-alive ID
                    m_lastKeepAliveId = ++m_keepAliveSequence;
                    SendKeepAlive(m_lastKeepAliveId);
                    m_awaitingKeepAlive = true;
                    Log::Info("[ServerConnection %u] Sent keep-alive with ID %llu",
                              GetConnectionId(), m_lastKeepAliveId);
                }
            } else {
                // Check for timeout on pending keep-alive
                if (now - m_lastKeepAliveSent >= CONNECTION_TIMEOUT) {
                    Log::Warning("[ServerConnection %u] Keep-alive timeout - no response to ID %llu", 
                                GetConnectionId(), m_lastKeepAliveId);
                    SendDisconnect("Timed out");
                    return;
                }
            }
        }
        
        // Check for timeout
        if (IsTimedOut()) {
            Log::Warning("[ServerConnection %u] Connection timed out", GetConnectionId());
            SendDisconnect("Timed out");
        }
    }
    
    void ServerConnection::setProtocolState(Network::ProtocolState state) {
        Log::Info("[ServerConnection %u] Switching protocol state to %d", 
                  GetConnectionId(), static_cast<int>(state));
        
        // Update phase (for legacy compatibility)
        switch (state) {
            case Network::ProtocolState::HANDSHAKING:
                m_phase = ConnectionPhase::HANDSHAKING;
                m_listener = std::make_unique<HandshakePacketListener>(*this);
                break;
                
            case Network::ProtocolState::STATUS:
                m_phase = ConnectionPhase::STATUS;
                // TODO: m_listener = std::make_unique<StatusPacketListener>(*this);
                Log::Warning("[ServerConnection %u] STATUS listener not implemented yet", GetConnectionId());
                break;
                
            case Network::ProtocolState::LOGIN:
                m_phase = ConnectionPhase::LOGIN;
                m_listener = std::make_unique<LoginPacketListener>(*this, m_server);
                break;
                
            case Network::ProtocolState::PLAY:
                // PLAY state requires a session to be passed via the overload
                Log::Error("[ServerConnection %u] Cannot switch to PLAY state without session", GetConnectionId());
                SendDisconnect("Server error: No session available for PLAY state");
                break;
        }
    }
    
    void ServerConnection::setProtocolState(Network::ProtocolState state, PlayerSession* session) {
        Log::Info("[ServerConnection %u] Switching protocol state to %d with session %u", 
                  GetConnectionId(), static_cast<int>(state), session ? session->GetPlayerId() : 0);
        
        // This overload is only for PLAY state with a session
        if (state != Network::ProtocolState::PLAY) {
            Log::Warning("[ServerConnection %u] Session-aware protocol switch only supports PLAY state", 
                        GetConnectionId());
            setProtocolState(state);  // Fall back to regular version
            return;
        }
        
        if (!session) {
            Log::Error("[ServerConnection %u] No session provided for PLAY state", 
                      GetConnectionId());
            SendDisconnect("Server error: No session available");
            return;
        }
        
        // Update phase
        m_phase = ConnectionPhase::PLAY;
        
        // Create listener with session reference
        m_listener = std::make_unique<ServerPlayPacketListener>(*this, *session);
        
        Log::Info("[ServerConnection %u] Switched to PLAY state with session-aware ServerPlayPacketListener", 
                 GetConnectionId());
    }
    
    void ServerConnection::sendInitialGameData() {
        Log::Debug("[ServerConnection %u] Sending initial game data", GetConnectionId());
        
        // Send time update
        SendTimeUpdate(0, 6000); // Noon
        
        // Send player abilities
        SendPlayerAbilities(0x0F, 0.05f, 0.1f); // All abilities, default speeds
        
        // Send spawn position
        Network::PacketBuffer spawnBuffer;
        spawnBuffer.WriteInt(0); // X
        spawnBuffer.WriteInt(67); // Y
        spawnBuffer.WriteInt(0); // Z
        SendPacket(static_cast<uint8_t>(Network::PacketId::WorldSpawn), spawnBuffer.GetData());
    }

    void ServerConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
        UpdateActivity();
        m_lastPacketReceived = std::chrono::steady_clock::now();
        
        // Forward to server for statistics
        if (m_server) {
            m_server->OnPacketReceived(
                std::static_pointer_cast<ServerConnection>(shared_from_this()),
                packetId, payload);
        }
        
        // Handle packet based on current phase
        if (!m_packetRegistry.HandlePacket(packetId, payload)) {
            Log::Warning("[ServerConnection %u] Unhandled packet ID: 0x%02X in phase %d",
                GetConnectionId(), packetId, static_cast<int>(m_phase));
        }
    }

    // ========================================================================
    // PACKET SENDING (SERVER → CLIENT)
    // ========================================================================

    void ServerConnection::SendBlockChange(const Network::BlockChangeS2CPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
    }

    void ServerConnection::SendChatMessage(const std::string& message, uint8_t position, uint32_t senderId) {
        Network::PacketBuffer buffer;
        buffer.WriteInt(static_cast<int32_t>(senderId)); // Sender player ID (0 = system)
        buffer.WriteString(message);
        buffer.WriteByte(position); // 0=chat, 1=system, 2=actionbar
        SendPacket(static_cast<uint8_t>(Network::PacketId::ChatMessageS2C), buffer.GetData());
    }

    void ServerConnection::SendKeepAlive(uint64_t id) {
        Network::PacketBuffer buffer;
        buffer.WriteLong(id);
        SendPacket(static_cast<uint8_t>(Network::PacketId::KeepAliveS2C), buffer.GetData());
        
        m_lastKeepAliveId = id;
        m_lastKeepAliveSent = std::chrono::steady_clock::now();
    }

    void ServerConnection::SendDisconnect(const std::string& reason) {
        Network::PacketBuffer buffer;
        buffer.WriteString(reason);
        SendPacket(static_cast<uint8_t>(Network::PacketId::Disconnect), buffer.GetData());
        
        // Give time for packet to send, then disconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Disconnect();
    }

    void ServerConnection::SendTimeUpdate(uint64_t worldAge, uint64_t timeOfDay) {
        Network::PacketBuffer buffer;
        buffer.WriteLong(worldAge);
        buffer.WriteLong(timeOfDay);
        SendPacket(static_cast<uint8_t>(Network::PacketId::TimeUpdate), buffer.GetData());
    }

    void ServerConnection::SendPlayerAbilities(uint8_t flags, float flySpeed, float walkSpeed) {
        Network::PacketBuffer buffer;
        buffer.WriteByte(flags);
        buffer.WriteFloat(flySpeed);
        buffer.WriteFloat(walkSpeed);
        SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAbilities), buffer.GetData());
    }

    // ========================================================================
    // PACKET HANDLERS (CLIENT → SERVER)
    // ========================================================================

    void ServerConnection::HandleHandshake(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::HANDSHAKING) {
            Log::Warning("[ServerConnection %u] Unexpected handshake in phase %d",
                GetConnectionId(), static_cast<int>(m_phase));
            return;
        }
        
        Network::PacketReader reader(payload);
        uint32_t protocolVersion = reader.ReadVarInt();
        std::string serverAddress = reader.ReadString();
        uint16_t serverPort = reader.ReadShort();
        uint32_t nextState = reader.ReadVarInt();
        
        Log::Info("[ServerConnection %u] Handshake: protocol=%u, address=%s:%u, nextState=%u",
            GetConnectionId(), protocolVersion, serverAddress.c_str(), serverPort, nextState);
        
        // Set next phase
        if (nextState == 1) {
            m_phase = ConnectionPhase::STATUS;
        } else if (nextState == 2) {
            m_phase = ConnectionPhase::LOGIN;
        } else {
            SendDisconnect("Invalid handshake state");
        }
    }

    void ServerConnection::HandleLoginStart(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::LOGIN) {
            Log::Warning("[ServerConnection %u] Unexpected login start in phase %d",
                GetConnectionId(), static_cast<int>(m_phase));
            return;
        }
        
        Network::PacketReader reader(payload);
        m_playerName = reader.ReadString();
        // Optional trailing colour byte (Game::PlayerColorId). Old clients don't
        // send it — those default to 0 (Default neon green).
        if (reader.Remaining() >= 1) {
            m_playerColor = reader.ReadByte();
        }

        Log::Info("[ServerConnection %u] Player login: %s (color id=%u)",
            GetConnectionId(), m_playerName.c_str(), static_cast<unsigned>(m_playerColor));
        
        // Simple authentication (accept everyone for now)
        m_authenticated = true;
        m_playerId = GetConnectionId(); // Use connection ID as player ID
        m_phase = ConnectionPhase::PLAY;
        
        Log::Info("[ServerConnection %u] Player '%s' AUTHENTICATED (m_authenticated=%s), phase=PLAY", 
                  GetConnectionId(), m_playerName.c_str(), m_authenticated ? "true" : "false");
        
        // Send login success
        Network::PacketBuffer buffer;
        buffer.WriteString(std::to_string(m_playerId)); // UUID as string
        buffer.WriteString(m_playerName);
        Log::Debug("[ServerConnection %u] Sending LoginSuccess packet", GetConnectionId());
        SendPacket(static_cast<uint8_t>(Network::PacketId::LoginSuccess), buffer.GetData());
        
        // Send initial game data
        Log::Debug("[ServerConnection %u] Sending initial game data", GetConnectionId());
        SendTimeUpdate(0, 6000); // Noon
        SendPlayerAbilities(0x0F, 0.05f, 0.1f); // All abilities, default speeds
        
        // Send spawn position
        Network::PacketBuffer spawnBuffer;
        spawnBuffer.WriteInt(0); // X
        spawnBuffer.WriteInt(67); // Y
        spawnBuffer.WriteInt(0); // Z
        Log::Debug("[ServerConnection %u] Sending WorldSpawn packet", GetConnectionId());
        SendPacket(static_cast<uint8_t>(Network::PacketId::WorldSpawn), spawnBuffer.GetData());
        
        // Notify server that player has joined and needs initial chunks
        if (m_server) {
            Log::Info("[ServerConnection %u] Notifying server of new player join", GetConnectionId());
            m_server->OnPlayerJoined(std::static_pointer_cast<ServerConnection>(shared_from_this()));
        }
    }

    void ServerConnection::HandleBlockAction(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) {
            return;
        }
        
        auto packet = Network::Serialization::DeserializeBlockActionC2S(payload);
        
        // Route through packet listener → PlayerSession::HandleBlockAction()
        // (validates against THIS player's position, not the host's)
        if (m_listener) {
            m_listener->onBlockActionC2S(packet);
        }
    }

    void ServerConnection::HandlePlayerMove(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) {
            return;
        }

        auto packet = Network::Serialization::DeserializePlayerMoveC2S(payload);

        // Route through packet listener → PlayerSession::HandlePlayerMove()
        if (m_listener) {
            m_listener->onPlayerMoveC2S(packet);
        }
    }

    void ServerConnection::HandleChatMessage(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) {
            return;
        }

        auto packet = Network::Serialization::DeserializeChatMessageC2S(payload);

        Log::Info("[Server#%u] RECEIVED ChatMessageC2S (ID: 0x%02X) - Message: %s (isCommand=%d)",
                  GetConnectionId(), static_cast<uint8_t>(Network::PacketId::ChatMessageC2S),
                  packet.message.c_str(), packet.isCommand);

        // Route commands to the dispatcher (MC: separate ServerboundChatCommandPacket)
        if (packet.isCommand && Server::g_integratedServer) {
            // Strip leading '/' if present
            std::string cmdLine = packet.message;
            if (!cmdLine.empty() && cmdLine[0] == '/') {
                cmdLine = cmdLine.substr(1);
            }

            auto* sessionManager = Server::g_integratedServer->GetSessionManager();
            if (!sessionManager) return;
            auto session = sessionManager->GetSession(m_playerId);
            if (session && session->GetPlayer()) {
                Server::g_integratedServer->GetCommandDispatcher().ExecuteCommand(
                    cmdLine, *session->GetPlayer(), *this, *sessionManager);
            }
            return; // Commands are NOT broadcast as chat
        }

        // Forward to IntegratedServer for processing
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ProcessChatMessage(packet);
        }

        // Broadcast chat to all connected players
        if (m_server) {
            std::string formattedMessage = "<" + m_playerName + "> " + packet.message;
            auto connections = m_server->GetConnections();

            std::vector<ServerConnectionPtr> activeConnections;
            for (auto& conn : connections) {
                if (conn && conn->GetState() != Network::ConnectionState::DISCONNECTED && conn->IsAuthenticated()) {
                    activeConnections.push_back(conn);
                }
            }

            for (auto& conn : activeConnections) {
                try {
                    conn->SendChatMessage(formattedMessage, 0, m_playerId);
                } catch (const std::exception& e) {
                    Log::Warning("Failed to send chat message to connection: %s", e.what());
                }
            }
        }
    }

    void ServerConnection::HandleKeepAliveResponse(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        uint64_t id = reader.ReadLong();
        
        if (id == m_lastKeepAliveId && m_awaitingKeepAlive) {
            m_awaitingKeepAlive = false;
            m_lastKeepAliveReceived = std::chrono::steady_clock::now();
            m_lastPacketReceived = m_lastKeepAliveReceived; // Reset the 15s timer
            
            // Calculate RTT if needed
            auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                m_lastKeepAliveReceived - m_lastKeepAliveSent).count();
            Log::Info("[Server#%u] RECEIVED KeepAliveC2S (ID: 0x%02X) - ID: %llu, RTT: %ldms",
                      GetConnectionId(), static_cast<uint8_t>(Network::PacketId::KeepAliveC2S), id, rtt);
        } else {
            Log::Warning("[ServerConnection %u] Unexpected keep-alive response (ID: %llu, expected: %llu)", 
                        GetConnectionId(), id, m_lastKeepAliveId);
        }
    }

    void ServerConnection::HandleHeldItemChange(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) return;

        auto packet = Network::Serialization::DeserializeHeldItemChangeC2S(payload);
        if (m_listener) {
            m_listener->onHeldItemChangeC2S(packet);
        }
    }

    void ServerConnection::HandleInventoryClick(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) return;
        auto packet = Network::Serialization::DeserializeInventoryClickC2S(payload);
        if (m_listener) m_listener->onInventoryClickC2S(packet);
    }

    void ServerConnection::HandleInventoryClose(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) return;
        auto packet = Network::Serialization::DeserializeInventoryCloseC2S(payload);
        if (m_listener) m_listener->onInventoryCloseC2S(packet);
    }

    void ServerConnection::Teleport(double x, double y, double z, float yRot, float xRot,
                                    double dx, double dy, double dz) {
        // Match MC's ServerGamePacketListenerImpl.teleport(PositionMoveRotation, Set<Relative>):
        //   1. Bump the awaiting-teleport id (wrap on int max)
        //   2. Snap the server-side player position
        //   3. Send ClientboundPlayerPosition to the client; client snaps and acks
        if (++m_awaitingTeleport == std::numeric_limits<int32_t>::max()) {
            m_awaitingTeleport = 0;
        }

        // Locate this connection's ServerPlayer via session manager and snap its position.
        // Use teleport() not setPosition() — setPosition() runs the anti-cheat
        // distance check (>100 blocks → reject + warn), which trips on every
        // legitimate server-issued teleport (portal jumps, /tp, world spawn
        // far from origin). teleport() bypasses the check.
        if (Server::g_integratedServer) {
            auto* sessionManager = Server::g_integratedServer->GetSessionManager();
            if (sessionManager) {
                auto session = sessionManager->GetSession(m_playerId);
                if (session && session->GetPlayer()) {
                    session->GetPlayer()->teleport(glm::dvec3(x, y, z));
                }
            }
        }

        Network::ClientboundPlayerPositionPacket packet;
        packet.id = m_awaitingTeleport;
        packet.x = x;  packet.y = y;  packet.z = z;
        packet.dx = dx; packet.dy = dy; packet.dz = dz;
        packet.yRot = yRot;
        packet.xRot = xRot;
        packet.relatives = 0; // empty Set<Relative> — fully absolute teleport
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ClientboundPlayerPosition), data);

        Log::Info("[ServerConnection %u] Teleport id=%d → (%.2f, %.2f, %.2f) vel(%.2f, %.2f, %.2f)",
                  GetConnectionId(), m_awaitingTeleport, x, y, z, dx, dy, dz);
    }

    void ServerConnection::HandleAcceptTeleportation(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) return;

        auto packet = Network::Serialization::DeserializeServerboundAcceptTeleportation(payload);
        if (packet.id == m_awaitingTeleport) {
            // MC: this is where awaitingPositionFromClient gets cleared so subsequent C2S position
            // packets are accepted again. Our server doesn't use that gate yet — log and move on.
            Log::Info("[ServerConnection %u] Teleport id=%d acked", GetConnectionId(), packet.id);
        } else {
            Log::Warning("[ServerConnection %u] Stale teleport ack: got %d, expected %d",
                         GetConnectionId(), packet.id, m_awaitingTeleport);
        }
    }

    void ServerConnection::HandleClientSettings(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        int renderDistance = reader.ReadVarInt();
        bool vsync = reader.ReadByte() != 0;
        float mouseSensitivity = reader.ReadFloat();

        // Clamp to valid range
        renderDistance = std::clamp(renderDistance, 2, 32);

        Log::Info("[Server#%u] Client settings: renderDistance=%d, vsync=%s, sensitivity=%.2f",
                  GetConnectionId(), renderDistance, vsync ? "true" : "false", mouseSensitivity);

        // Forward to IntegratedServer for per-player view distance update
        if (Server::g_integratedServer) {
            Server::g_integratedServer->OnClientSettingsReceived(GetConnectionId(), renderDistance);
        }
    }

    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    bool ServerConnection::ValidatePacketSize(const std::vector<uint8_t>& payload, size_t expectedMin) {
        if (payload.size() < expectedMin) {
            Log::Warning("[ServerConnection %u] Packet too small: %zu < %zu",
                GetConnectionId(), payload.size(), expectedMin);
            return false;
        }
        return true;
    }

    bool ServerConnection::CheckRateLimit(const std::string& action) {
        auto now = std::chrono::steady_clock::now();
        auto& limit = m_rateLimits[action];
        
        // Reset counter every second
        if (now > limit.resetTime) {
            limit.count = 0;
            limit.resetTime = now + std::chrono::seconds(1);
        }
        
        // Check limit (e.g., 10 actions per second)
        if (limit.count >= 10) {
            Log::Warning("[ServerConnection %u] Rate limit exceeded for %s",
                GetConnectionId(), action.c_str());
            return false;
        }
        
        limit.count++;
        return true;
    }

    bool ServerConnection::IsTimedOut() const {
        auto now = std::chrono::steady_clock::now();
        
        if (m_phase == ConnectionPhase::LOGIN) {
            return (now - m_lastPacketReceived) > LOGIN_TIMEOUT;
        } else if (m_phase == ConnectionPhase::PLAY) {
            // During play, timeout based on last packet received (any packet counts)
            return (now - m_lastPacketReceived) > (CONNECTION_TIMEOUT * 2);  // Be generous, 60s
        }
        
        return false;
    }
    
    // Decode packet on I/O thread (override from base class)
    Network::PacketPtr ServerConnection::DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        using namespace Network;
        
        // Create typed packets based on packet ID and current protocol state
        // This runs on the I/O thread, so we just create the packet object
        // The actual handling happens on the server thread via tick()
        
        PacketReader reader(payload);
        
        switch (static_cast<PacketId>(packetId)) {
            case PacketId::Handshake:
                if (m_phase == ConnectionPhase::HANDSHAKING) {
                    auto packet = std::make_unique<HandshakeC2SPacket>(reader);
                    
                    // CRITICAL: Switch protocol state NOW on I/O thread
                    // This ensures the next packet (LoginStart) is decoded with correct state
                    // This is exactly how Minecraft/Netty handles it
                    if (packet->nextState == static_cast<int32_t>(NextStateWire::LOGIN)) {
                        Log::Debug("[ServerConnection %u] I/O thread switching to LOGIN state", GetConnectionId());
                        m_phase = ConnectionPhase::LOGIN;
                        // Note: Listener will be created on server thread when packet is processed
                    } else if (packet->nextState == static_cast<int32_t>(NextStateWire::STATUS)) {
                        Log::Debug("[ServerConnection %u] I/O thread switching to STATUS state", GetConnectionId());
                        m_phase = ConnectionPhase::STATUS;
                    }
                    
                    return packet;
                }
                break;
                
            case PacketId::LoginStart:
                if (m_phase == ConnectionPhase::LOGIN) {
                    return std::make_unique<LoginStartC2SPacket>(reader);
                }
                break;
                
            case PacketId::KeepAliveC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    return std::make_unique<KeepAliveC2SPacket>(reader);
                }
                break;
            
            case PacketId::UseItemOnC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeUseItemOnC2S(payload);
                    return std::make_unique<Network::Packets::UseItemOnC2SPacketImpl>(std::move(data));
                }
                break;
            
            case PacketId::ChunkBatchAckC2S:
                if (m_phase == ConnectionPhase::PLAY) {
                    auto data = Network::Serialization::DeserializeChunkBatchAckC2S(payload);
                    return std::make_unique<Network::Packets::ChunkBatchAckC2SPacketImpl>(data.desiredChunksPerTick);
                }
                break;

            // TODO: Add more packet types as we implement them
            // case PacketId::BlockActionC2S:
            // case PacketId::PlayerMoveC2S:
            // case PacketId::ChatMessageC2S:

            default:
                // For packets we haven't converted yet, return nullptr to fall back to legacy
                return nullptr;
        }
        
        // Unknown or invalid packet for current state
        Log::Warning("[ServerConnection %u] Unexpected packet 0x%02X in state %d", 
                    GetConnectionId(), packetId, static_cast<int>(m_phase));
        return nullptr;
    }

} // namespace Server