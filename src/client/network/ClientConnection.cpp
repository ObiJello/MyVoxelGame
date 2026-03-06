// File: src/client/network/ClientConnection.cpp
#include "ClientConnection.hpp"
#include "NetworkClient.hpp"
#include "common/core/Log.hpp"
#include "common/network/packets/S2CPackets.hpp"  // Ensure packet implementations are available
#include "../world/ClientChunkManager.hpp"

namespace Client {

    ClientConnection::ClientConnection(tcp::socket socket, NetworkClient* client)
        : NetworkConnection(std::move(socket))
        , m_client(client)
    {
        // Set connection name to "Client" for clearer logging
        SetName("Client");
        
        // Register packet handlers for server → client packets
        using namespace Network;
        m_packetRegistry.RegisterHandler(PacketId::LoginSuccess,
            [this](const std::vector<uint8_t>& p) { HandleLoginSuccess(p); });
        m_packetRegistry.RegisterHandler(PacketId::Disconnect,
            [this](const std::vector<uint8_t>& p) { HandleDisconnect(p); });
        m_packetRegistry.RegisterHandler(PacketId::BlockChangeS2C,
            [this](const std::vector<uint8_t>& p) { HandleBlockChange(p); });
        m_packetRegistry.RegisterHandler(PacketId::ChatMessageS2C,
            [this](const std::vector<uint8_t>& p) { HandleChatMessage(p); });
        m_packetRegistry.RegisterHandler(PacketId::TimeUpdate,
            [this](const std::vector<uint8_t>& p) { HandleTimeUpdate(p); });
        // KeepAliveS2C is now handled through typed packet system (KeepAliveS2CPacketImpl)
        // m_packetRegistry.RegisterHandler(PacketId::KeepAliveS2C,
        //     [this](const std::vector<uint8_t>& p) { HandleKeepAlive(p); });
        m_packetRegistry.RegisterHandler(PacketId::PlayerAbilities,
            [this](const std::vector<uint8_t>& p) { HandlePlayerAbilities(p); });
        m_packetRegistry.RegisterHandler(PacketId::WorldSpawn,
            [this](const std::vector<uint8_t>& p) { HandleWorldSpawn(p); });
    }

    ClientConnection::~ClientConnection() {
    }

    void ClientConnection::OnConnected() {
        Log::Info("[ClientConnection] Connected to server");
    }

    void ClientConnection::OnDisconnected() {
        Log::Info("[ClientConnection] Disconnected from server");
        
        if (m_client) {
            m_client->OnConnectionClosed("Connection lost");
        }
    }

    void ClientConnection::OnError(const error_code& error) {
        Log::Error("[ClientConnection] Error: %s", error.message().c_str());
        
        if (m_client) {
            m_client->OnConnectionError(error.message());
        }
    }

    void ClientConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
        // Debug: Log all packet receptions to see if client is receiving anything
        Log::Info("[ClientConnection] PACKET RECEIVED: ID=0x%02X, Size=%zu bytes", packetId, payload.size());
        
        // Add special logging for important packets
        if (packetId == 0x20) {
            Log::Info("[ClientConnection] *** CHUNK DATA PACKET RECEIVED! *** ID=0x20, Size=%zu bytes", payload.size());
        } else if (packetId == 0x06) {
            Log::Info("[ClientConnection] *** LOGIN SUCCESS PACKET RECEIVED! *** ID=0x06, Size=%zu bytes", payload.size());
        }
        
        // Forward to client for statistics
        if (m_client) {
            m_client->OnPacketReceived(packetId, payload);
        }
        
        // Handle packet
        if (!m_packetRegistry.HandlePacket(packetId, payload)) {
            Log::Warning("[ClientConnection] Unhandled packet ID: 0x%02X", packetId);
        }
    }

    void ClientConnection::StartHandshake(const std::string& playerName, uint16_t serverPort) {
        m_playerName = playerName;
        m_phase = ConnectionPhase::HANDSHAKING;

        // Send handshake packet
        Network::PacketBuffer buffer;
        buffer.WriteVarInt(754); // Protocol version (1.16.5)
        buffer.WriteString("127.0.0.1"); // Server address
        buffer.WriteShort(serverPort); // Server port
        buffer.WriteVarInt(2); // Next state: LOGIN
        SendPacket(static_cast<uint8_t>(Network::PacketId::Handshake), buffer.GetData());
        
        m_phase = ConnectionPhase::LOGIN;
        
        // Send login start packet
        Network::PacketBuffer loginBuffer;
        loginBuffer.WriteString(playerName);
        SendPacket(static_cast<uint8_t>(Network::PacketId::LoginStart), loginBuffer.GetData());
        
        Log::Info("[ClientConnection] Sent handshake and login for player: %s", playerName.c_str());
    }

    // ========================================================================
    // PACKET SENDING (CLIENT → SERVER)
    // ========================================================================

    void ClientConnection::SendBlockAction(const Network::BlockActionC2SPacket& packet) {
        Log::Info("[Client] SENDING BlockActionC2S (ID: 0x%02X) - Action: %d, Pos: (%d, %d, %d)",
                  static_cast<uint8_t>(Network::PacketId::BlockActionC2S), 
                  packet.action, packet.worldX, packet.worldY, packet.worldZ);
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
    }

    void ClientConnection::SendPlayerMove(const Network::PlayerMoveC2SPacket& packet) {
        // Only log non-keep-alive player moves to reduce spam
        if (packet.sequenceNumber % 20 == 0) {  // Log every 20th move
            /*Log::Debug("[Client] SENDING PlayerMoveC2S (ID: 0x%02X) - Pos: (%.2f, %.2f, %.2f)",
                      static_cast<uint8_t>(Network::PacketId::PlayerMoveC2S),
                      packet.position.x, packet.position.y, packet.position.z);*/
        }
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerMoveC2S), data);
    }

    void ClientConnection::SendChatMessage(const std::string& message) {
        Network::ChatMessageC2SPacket packet;
        packet.message = message;
        packet.timestamp = static_cast<uint32_t>(std::time(nullptr));
        packet.isCommand = !message.empty() && message[0] == '/';
        
        Log::Info("[Client] SENDING ChatMessageC2S (ID: 0x%02X) - Message: %s",
                  static_cast<uint8_t>(Network::PacketId::ChatMessageC2S), message.c_str());
        
        auto data = Network::Serialization::Serialize(packet);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ChatMessageC2S), data);
    }

    void ClientConnection::SendClientSettings(int renderDistance, bool vsync, float mouseSensitivity) {
        Log::Info("[Client] SENDING ClientConfigC2S (ID: 0x%02X) - RenderDist: %d, VSync: %s",
                  static_cast<uint8_t>(Network::PacketId::ClientConfigC2S), 
                  renderDistance, vsync ? "true" : "false");
        
        Network::PacketBuffer buffer;
        buffer.WriteVarInt(renderDistance);
        buffer.WriteByte(vsync ? 1 : 0);
        buffer.WriteFloat(mouseSensitivity);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ClientConfigC2S), buffer.GetData());
    }

    void ClientConnection::SendKeepAliveResponse(uint64_t id) {
        Log::Debug("[Client] SENDING KeepAliveC2S (ID: 0x%02X) - ID: %llu",
                   static_cast<uint8_t>(Network::PacketId::KeepAliveC2S), id);
        
        Network::PacketBuffer buffer;
        buffer.WriteLong(id);
        SendPacket(static_cast<uint8_t>(Network::PacketId::KeepAliveC2S), buffer.GetData());
    }

    // ========================================================================
    // PACKET HANDLERS (SERVER → CLIENT)
    // ========================================================================

    void ClientConnection::HandleLoginSuccess(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        std::string uuid = reader.ReadString();
        std::string username = reader.ReadString();
        
        Log::Info("[ClientConnection] LOGIN SUCCESS RECEIVED! User: %s (UUID: %s)", 
            username.c_str(), uuid.c_str());
        
        m_playerId = std::stoul(uuid); // Simple conversion for now
        m_loggedIn = true;
        m_phase = ConnectionPhase::PLAY;
        
        // Send client settings
        SendClientSettings(8, true, 1.0f);
    }

    void ClientConnection::HandleDisconnect(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        std::string reason = reader.ReadString();
        
        Log::Info("[ClientConnection] Disconnected by server: %s", reason.c_str());
        
        Disconnect();
        
        if (m_client) {
            m_client->OnConnectionClosed(reason);
        }
    }

    void ClientConnection::HandleBlockChange(const std::vector<uint8_t>& payload) {
        auto packet = Network::Serialization::DeserializeBlockChangeS2C(payload);
        
        // Queue packet for main thread processing via IncomingPacket queue
        // The packet will be processed by ClientPacketHandler on the main thread
        Log::Debug("[ClientConnection] Received block change at (%d, %d, %d) -> block %d",
                   packet.worldX, packet.worldY, packet.worldZ, static_cast<int>(packet.newBlockId));
    }

    void ClientConnection::HandleChatMessage(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        std::string message = reader.ReadString();
        uint8_t position = reader.ReadByte();
        
        const char* positionStr = "";
        switch (position) {
            case 0: positionStr = "[CHAT]"; break;
            case 1: positionStr = "[SYSTEM]"; break;
            case 2: positionStr = "[ACTION]"; break;
        }
        
        Log::Info("%s %s", positionStr, message.c_str());
    }

    void ClientConnection::HandleTimeUpdate(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        m_worldAge = reader.ReadLong();
        m_timeOfDay = reader.ReadLong();
        
        Log::Debug("[ClientConnection] Time update: age=%lu, time=%lu", 
            m_worldAge, m_timeOfDay);
    }

    void ClientConnection::HandlePlayerAbilities(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        m_playerAbilities = reader.ReadByte();
        m_flySpeed = reader.ReadFloat();
        m_walkSpeed = reader.ReadFloat();
        
        Log::Debug("[ClientConnection] Player abilities: flags=0x%02X, fly=%.2f, walk=%.2f",
            m_playerAbilities, m_flySpeed, m_walkSpeed);
    }

    void ClientConnection::HandleWorldSpawn(const std::vector<uint8_t>& payload) {
        Network::PacketReader reader(payload);
        m_spawnPosition.x = reader.ReadInt();
        m_spawnPosition.y = reader.ReadInt();
        m_spawnPosition.z = reader.ReadInt();
        
        Log::Info("[ClientConnection] World spawn set to (%.0f, %.0f, %.0f)",
            m_spawnPosition.x, m_spawnPosition.y, m_spawnPosition.z);
    }

    // ========================================================================
    // PACKET DECODING (I/O THREAD)
    // ========================================================================
    
    Network::PacketPtr ClientConnection::DecodePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
        using namespace Network;
        using namespace Network::Packets;
        
        // Create typed packet on I/O thread based on packet ID
        switch (static_cast<PacketId>(packetId)) {
            case PacketId::ChunkDataS2C: {
                auto data = Serialization::DeserializeChunkDataS2C(payload);
                return std::make_unique<ChunkDataS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::UnloadChunkS2C: {
                auto data = Serialization::DeserializeUnloadChunkS2C(payload);
                return std::make_unique<UnloadChunkS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::BlockChangeS2C: {
                auto data = Serialization::DeserializeBlockChangeS2C(payload);
                return std::make_unique<BlockChangeS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::MultiBlockChangeS2C: {
                auto data = Serialization::DeserializeMultiBlockChangeS2C(payload);
                return std::make_unique<MultiBlockChangeS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::PlayerUpdateS2C: {
                auto data = Serialization::DeserializePlayerUpdateS2C(payload);
                return std::make_unique<PlayerUpdateS2CPacketImpl>(std::move(data));
            }
            
            case PacketId::Disconnect: {
                PacketReader reader(payload);
                std::string reason = reader.ReadString();
                return std::make_unique<DisconnectPacketImpl>(std::move(reason));
            }
            
            case PacketId::KeepAliveS2C: {
                PacketReader reader(payload);
                uint64_t id = reader.ReadLong();
                return std::make_unique<KeepAliveS2CPacketImpl>(id);
            }
            
            default:
                // Return nullptr for unhandled packets - will fall back to legacy OnPacketReceived
                return nullptr;
        }
    }
    
    // ========================================================================
    // MAIN THREAD PACKET PROCESSING
    // ========================================================================
    
    void ClientConnection::DrainIncomingPackets() {
        if (!m_client) return;
        
        // Get packet handler from client
        auto handler = m_client->GetPacketHandler();
        if (!handler) return;
        
        // Process packets in batches to avoid stalling
        size_t processed = 0;
        
        Network::IncomingPacket packet;
        while (TryPopIncoming(packet)) {
            try {
                // Apply packet through visitor pattern (main thread only)
                if (auto* s2cPacket = dynamic_cast<Network::IS2CPacket*>(packet.packet.get())) {
                    // Debug log for ChunkDataS2C packets
                    if (packet.packet->getId() == Network::PacketId::ChunkDataS2C) {
                        Log::Info("[ClientConnection] Processing ChunkDataS2C packet from queue");
                    }
                    s2cPacket->apply(*handler);
                    processed++;  // Only count as processed if successfully handled
                } else {
                    // Debug: packet couldn't be cast to IS2CPacket
                    if (packet.packet) {
                        Log::Warning("[ClientConnection] Failed to cast packet ID 0x%02X to IS2CPacket", 
                                   static_cast<int>(packet.packet->getId()));
                    } else {
                        Log::Warning("[ClientConnection] Null packet in queue");
                    }
                }
            } catch (const std::exception& e) {
                Log::Error("[ClientConnection] Exception applying packet: %s", e.what());
            }
        }
        
        if (processed > 0) {
            Log::Debug("[ClientConnection] Processed %zu packets this frame", processed);
        }
        
        // Log if queue is getting full
        size_t queueSize = GetIncomingQueueSize();
        if (queueSize > 1000) {
            Log::Warning("[ClientConnection] Incoming packet queue large: %zu packets", queueSize);
        }
    }

} // namespace Client