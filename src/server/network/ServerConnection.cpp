// File: src/server/network/ServerConnection.cpp
#include "ServerConnection.hpp"
#include "NetworkServer.hpp"
#include "listeners/HandshakePacketListener.hpp"
#include "listeners/LoginPacketListener.hpp"
#include "listeners/ServerPlayPacketListener.hpp"
#include "common/core/Log.hpp"
#include "common/network/packets/HandshakeC2S.hpp"
#include "common/network/packets/LoginStartC2S.hpp"
#include "common/network/packets/KeepAliveC2S.hpp"
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
                m_server->OnConnectionClosed(
                    std::static_pointer_cast<ServerConnection>(shared_from_this()));
            } catch (const std::exception& e) {
                // Log but don't rethrow - this can be called during destruction
                Log::Warning("[ServerConnection %u] Failed to notify server of disconnection: %s", 
                    GetConnectionId(), e.what());
            }
        }
    }

    void ServerConnection::OnError(const boost::system::error_code& error) {
        Log::Error("[ServerConnection %u] Error: %s", 
            GetConnectionId(), error.message().c_str());
    }
    
    void ServerConnection::tick() {
        // Drain incoming packets queue and apply to listener
        Network::IncomingPacket packet;
        int packetsProcessed = 0;
        const int MAX_PACKETS_PER_TICK = 1000;  // Safety limit only - time budget is primary control
        const float INBOUND_PROCESS_BUDGET_MS = 0.5f;  // Time budget for processing inbound packets
        
        auto startTime = std::chrono::steady_clock::now();
        
        while (packetsProcessed < MAX_PACKETS_PER_TICK && TryPopIncoming(packet)) {
            // Check time budget
            auto currentTime = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(currentTime - startTime).count();
            if (elapsedMs >= INBOUND_PROCESS_BUDGET_MS) {
                break;  // Time budget exceeded
            }
            if (packet.packet) {
                // Check if we need to create a listener based on the packet type
                // This happens on server thread after I/O thread has already switched protocol state
                if (!m_listener) {
                    if (packet.packet->getId() == Network::PacketId::Handshake) {
                        // Handshake packet - listener will be created by setProtocolState
                        // which is called by the HandshakePacketListener
                        m_listener = std::make_unique<HandshakePacketListener>(*this);
                    } else if (packet.packet->getId() == Network::PacketId::LoginStart && 
                               m_phase == ConnectionPhase::LOGIN) {
                        // LoginStart arrived but no listener yet - create it
                        Log::Debug("[ServerConnection %u] Creating LoginPacketListener for queued LoginStart", 
                                   GetConnectionId());
                        m_listener = std::make_unique<LoginPacketListener>(*this, m_server);
                    }
                }
                
                if (m_listener) {
                    try {
                        // Apply packet to current listener (visitor pattern)
                        if (auto* c2sPacket = dynamic_cast<Network::IC2SPacket*>(packet.packet.get())) {
                            c2sPacket->apply(*m_listener);
                            packetsProcessed++;
                        } else {
                            Log::Warning("[ServerConnection %u] Packet is not a C2S packet", GetConnectionId());
                        }
                    } catch (const std::exception& e) {
                        Log::Error("[ServerConnection %u] Exception processing packet: %s", 
                                  GetConnectionId(), e.what());
                    }
                }
            }
        }
        
        if (packetsProcessed > 0) {
            Log::Debug("[ServerConnection %u] Processed %d packets this tick", 
                      GetConnectionId(), packetsProcessed);
        }
        
        // Send periodic keep-alives during PLAY phase
        if (m_phase == ConnectionPhase::PLAY) {
            auto now = std::chrono::steady_clock::now();
            
            if (!m_awaitingKeepAlive) {
                // Send new keep-alive if interval has passed since last packet received
                if (now - m_lastPacketReceived >= KEEP_ALIVE_INTERVAL) {
                    // Generate a new keep-alive ID
                    m_lastKeepAliveId = ++m_keepAliveSequence;
                    SendKeepAlive(m_lastKeepAliveId);
                    m_awaitingKeepAlive = true;
                    Log::Debug("[ServerConnection %u] Sent keep-alive with ID %llu", 
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
                m_phase = ConnectionPhase::PLAY;
                m_listener = std::make_unique<ServerPlayPacketListener>(*this);
                Log::Info("[ServerConnection %u] Switched to PLAY state with ServerPlayPacketListener", 
                         GetConnectionId());
                break;
        }
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

    void ServerConnection::SendChunkData(const Network::ServerChunkDataPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ServerChunkData), data);
    }

    void ServerConnection::SendChunkUnload(const Network::ServerChunkUnloadPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ServerChunkUnload), data);
    }

    void ServerConnection::SendBlockChange(const Network::BlockChangeS2CPacket& packet) {
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
    }

    void ServerConnection::SendChatMessage(const std::string& message, uint8_t position) {
        Network::PacketBuffer buffer;
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
        
        Log::Info("[ServerConnection %u] Player login: %s", 
            GetConnectionId(), m_playerName.c_str());
        
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
        
        Log::Debug("[ServerConnection %u] Block action at (%d,%d,%d): %d",
            GetConnectionId(), packet.worldX, packet.worldY, packet.worldZ,
            static_cast<int>(packet.action));
        
        // Forward to IntegratedServer for processing
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ProcessBlockAction(packet);
        }
    }

    void ServerConnection::HandlePlayerMove(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) {
            return;
        }
        
        auto packet = Network::Serialization::DeserializePlayerMoveC2S(payload);
        
        // Forward to IntegratedServer for processing
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ProcessPlayerMove(packet);
        }
    }

    void ServerConnection::HandleChatMessage(const std::vector<uint8_t>& payload) {
        if (m_phase != ConnectionPhase::PLAY || !m_authenticated) {
            return;
        }
        
        auto packet = Network::Serialization::DeserializeChatMessageC2S(payload);
        
        // Forward to IntegratedServer for processing
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ProcessChatMessage(packet);
        }
        
        // Also broadcast to all connected players
        if (m_server) {
            std::string formattedMessage = "<" + m_playerName + "> " + packet.message;
            auto connections = m_server->GetConnections();
            for (auto& conn : connections) {
                if (conn->IsAuthenticated()) {
                    conn->SendChatMessage(formattedMessage, 0);
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
            Log::Debug("[ServerConnection %u] Keep-alive response received (ID: %llu, RTT: %ldms)", 
                      GetConnectionId(), id, rtt);
        } else {
            Log::Warning("[ServerConnection %u] Unexpected keep-alive response (ID: %llu, expected: %llu)", 
                        GetConnectionId(), id, m_lastKeepAliveId);
        }
    }

    void ServerConnection::HandleClientSettings(const std::vector<uint8_t>& payload) {
        // Parse client settings (render distance, etc.)
        // This would update per-player settings
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
                    if (packet->nextState == ProtocolState::LOGIN) {
                        Log::Debug("[ServerConnection %u] I/O thread switching to LOGIN state", GetConnectionId());
                        m_phase = ConnectionPhase::LOGIN;
                        // Note: Listener will be created on server thread when packet is processed
                    } else if (packet->nextState == ProtocolState::STATUS) {
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