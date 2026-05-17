// File: src/client/network/ClientConnection.cpp
#include "ClientConnection.hpp"
#include "NetworkClient.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/network/packets/S2CPackets.hpp"  // Ensure packet implementations are available
#include "../world/ClientChunkManager.hpp"
#include "../entity/RemotePlayerManager.hpp"
#include "platform/GameDirectory.hpp"
#include <functional>

// Chat message callback — set by PlatformMain to route messages to ChatComponent
static std::function<void(const std::string&)> s_chatCallback;
static std::function<void(uint32_t, const std::string&)> s_chatBubbleCallback;
// Teleport callback — set by PlatformMain to snap the local Player on /tp.
// Signature: (x, y, z, yaw, pitch, dx, dy, dz). dx/dy/dz are the
// authoritative post-teleport velocity in blocks/sec, matching the
// packet's deltaMovement field (MC convention). For /tp the server
// passes zero (kills momentum); for portal teleports it passes the
// rotated source velocity (carries momentum through the portal pair).
static std::function<void(double, double, double, float, float,
                          double, double, double)> s_teleportCallback;

void SetChatMessageCallback(std::function<void(const std::string&)> callback) {
    s_chatCallback = std::move(callback);
}

void SetChatBubbleCallback(std::function<void(uint32_t, const std::string&)> callback) {
    s_chatBubbleCallback = std::move(callback);
}

void SetTeleportCallback(std::function<void(double, double, double, float, float,
                                             double, double, double)> callback) {
    s_teleportCallback = std::move(callback);
}

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
        m_packetRegistry.RegisterHandler(PacketId::PlayerInfoS2C,
            [this](const std::vector<uint8_t>& p) { HandlePlayerInfo(p); });
        m_packetRegistry.RegisterHandler(PacketId::ClientboundPlayerPosition,
            [this](const std::vector<uint8_t>& p) { HandleClientboundPlayerPosition(p); });
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

    void ClientConnection::StartHandshake(const std::string& playerName,
                                          uint8_t playerColor,
                                          const std::string& serverHost,
                                          uint16_t serverPort) {
        m_playerName = playerName;
        m_phase = ConnectionPhase::HANDSHAKING;

        // Send handshake packet
        Network::PacketBuffer buffer;
        buffer.WriteVarInt(754); // Protocol version (1.16.5)
        buffer.WriteString(serverHost); // Server address
        buffer.WriteShort(serverPort); // Server port
        buffer.WriteVarInt(2); // Next state: LOGIN
        SendPacket(static_cast<uint8_t>(Network::PacketId::Handshake), buffer.GetData());

        m_phase = ConnectionPhase::LOGIN;

        // Send login start packet — name + colorId (1 byte). Tail-appending the
        // colorId means an old server that doesn't read it just sees the name and
        // ignores the extra byte; a new server reading from an old client gets
        // remaining()==0 after the name and falls back to Default colour.
        Network::PacketBuffer loginBuffer;
        loginBuffer.WriteString(playerName);
        loginBuffer.WriteByte(playerColor);
        SendPacket(static_cast<uint8_t>(Network::PacketId::LoginStart), loginBuffer.GetData());

        Log::Info("[ClientConnection] Sent handshake and login for player: %s (color id=%u)",
                  playerName.c_str(), static_cast<unsigned>(playerColor));
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
        
        // Send client settings with actual render distance from game settings
        SendClientSettings(
            Platform::g_gameSettings.GetRenderDistance(),
            Platform::g_gameSettings.GetVSync(),
            Platform::g_gameSettings.GetMouseSensitivity()
        );
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
        uint32_t senderId = static_cast<uint32_t>(reader.ReadInt());
        std::string message = reader.ReadString();
        uint8_t position = reader.ReadByte();

        const char* positionStr = "";
        switch (position) {
            case 0: positionStr = "[CHAT]"; break;
            case 1: positionStr = "[SYSTEM]"; break;
            case 2: positionStr = "[ACTION]"; break;
        }

        Log::Info("%s %s", positionStr, message.c_str());

        // Add to chat HUD
        if (s_chatCallback) {
            s_chatCallback(message);
        }

        // Set chat bubble on the remote player (not on self)
        if (s_chatBubbleCallback && senderId != 0) {
            s_chatBubbleCallback(senderId, message);
        }
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

    void ClientConnection::HandleClientboundPlayerPosition(const std::vector<uint8_t>& payload) {
        auto packet = Network::Serialization::DeserializeClientboundPlayerPosition(payload);

        // MC's client computes absolute values from current player state when relative bits are
        // set. Our /tp only ever sends absolute (relatives == 0), so the snap is straightforward.
        // If we ever support relative teleport, this is where to apply Relative bit logic.
        if (s_teleportCallback) {
            s_teleportCallback(packet.x, packet.y, packet.z,
                               packet.yRot, packet.xRot,
                               packet.dx, packet.dy, packet.dz);
        } else {
            Log::Warning("[ClientConnection] Got teleport packet but no callback registered");
        }

        // Echo the id back so the server's awaiting-teleport gate clears
        // (MC: ServerboundAcceptTeleportationPacket).
        Network::ServerboundAcceptTeleportationPacket ack;
        ack.id = packet.id;
        auto data = Network::Serialization::Serialize(ack);
        SendPacket(static_cast<uint8_t>(Network::PacketId::ServerboundAcceptTeleportation), data);

        Log::Info("[ClientConnection] Teleport id=%d → (%.2f, %.2f, %.2f), acked",
                  packet.id, packet.x, packet.y, packet.z);
    }

    void ClientConnection::HandlePlayerInfo(const std::vector<uint8_t>& payload) {
        auto packet = Network::Serialization::DeserializePlayerInfoS2C(payload);

        // Don't track our own player ID in the remote player manager — it's only for OTHER players
        // (matching MC: the local player is in PlayerInfo for tab-list purposes, but we don't render
        // ourselves as a remote entity).
        if (packet.playerId == m_playerId) {
            Log::Info("[ClientConnection] PlayerInfo: ignoring entry for self (ID: %u)", packet.playerId);
            return;
        }

        if (!Client::g_remotePlayerManager) return;

        if (packet.action == Network::PlayerInfoS2CPacket::Action::ADD) {
            Client::g_remotePlayerManager->SetPlayerName(packet.playerId, packet.playerName);
            Client::g_remotePlayerManager->SetPlayerColor(
                packet.playerId,
                static_cast<Game::PlayerColorId>(packet.colorId));
            Log::Info("[ClientConnection] PlayerInfo ADD: '%s' (ID: %u, color id=%u)",
                      packet.playerName.c_str(), packet.playerId,
                      static_cast<unsigned>(packet.colorId));
        } else if (packet.action == Network::PlayerInfoS2CPacket::Action::REMOVE) {
            Client::g_remotePlayerManager->RemovePlayer(packet.playerId);
            Log::Info("[ClientConnection] PlayerInfo REMOVE: ID %u", packet.playerId);
        }
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

            case PacketId::EntityDestroy: {
                auto data = Serialization::DeserializeRemoveEntitiesS2C(payload);
                return std::make_unique<RemoveEntitiesS2CPacketImpl>(std::move(data));
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

            case PacketId::ChunkBatchStartS2C: {
                return std::make_unique<ChunkBatchStartS2CPacketImpl>();
            }

            case PacketId::ChunkBatchFinishedS2C: {
                auto data = Serialization::DeserializeChunkBatchFinishedS2C(payload);
                return std::make_unique<ChunkBatchFinishedS2CPacketImpl>(data.batchSize);
            }

            case PacketId::HotbarSyncS2C: {
                auto data = Serialization::DeserializeHotbarSyncS2C(payload);
                return std::make_unique<HotbarSyncS2CPacketImpl>(std::move(data));
            }

            case PacketId::SetChunkCacheRadiusS2C: {
                auto data = Serialization::DeserializeSetChunkCacheRadiusS2C(payload);
                return std::make_unique<SetChunkCacheRadiusS2CPacketImpl>(data.viewDistance);
            }

            case PacketId::InventoryFullS2C: {
                auto data = Serialization::DeserializeInventoryFullS2C(payload);
                return std::make_unique<InventoryFullS2CPacketImpl>(std::move(data));
            }

            case PacketId::InventorySetSlotS2C: {
                auto data = Serialization::DeserializeInventorySetSlotS2C(payload);
                return std::make_unique<InventorySetSlotS2CPacketImpl>(data);
            }

            case PacketId::InventorySetCarriedS2C: {
                auto data = Serialization::DeserializeInventorySetCarriedS2C(payload);
                return std::make_unique<InventorySetCarriedS2CPacketImpl>(data);
            }

#if ENABLE_PORTAL_GUN
            case PacketId::PortalSetS2C: {
                auto data = Serialization::DeserializePortalSetS2C(payload);
                return std::make_unique<PortalSetS2CPacketImpl>(data);
            }

            case PacketId::PortalRemoveS2C: {
                auto data = Serialization::DeserializePortalRemoveS2C(payload);
                return std::make_unique<PortalRemoveS2CPacketImpl>(data);
            }

            case PacketId::PortalTeleportFlashS2C: {
                auto data = Serialization::DeserializePortalTeleportFlashS2C(payload);
                return std::make_unique<PortalTeleportFlashS2CPacketImpl>(data);
            }

            case PacketId::PortalFizzleS2C: {
                auto data = Serialization::DeserializePortalFizzleS2C(payload);
                return std::make_unique<PortalFizzleS2CPacketImpl>(data);
            }
#endif

            default:
                // Return nullptr for unhandled packets - will fall back to legacy OnPacketReceived
                return nullptr;
        }
    }
    
    // ========================================================================
    // MAIN THREAD PACKET PROCESSING
    // ========================================================================
    
    void ClientConnection::DrainIncomingPackets() {
        PROFILE_ZONE;
        if (!m_client) return;

        auto handler = m_client->GetPacketHandler();
        if (!handler) return;

        // Drain ALL queued packets — matches Minecraft's PacketProcessor.processQueuedPackets().
        // No budget check needed: the client tick (20 TPS) naturally limits how many packets
        // accumulate per drain, and the server's ChunkBatchSizeCalculator + back-pressure
        // limits chunk data throughput to ~7ms per tick.
        Network::IncomingPacket packet;
        while (TryPopIncoming(packet)) {
            try {
                if (auto* s2cPacket = dynamic_cast<Network::IS2CPacket*>(packet.packet.get())) {
                    PROFILE_ZONE_N("ApplyPacket");
                    s2cPacket->apply(*handler);
                }
            } catch (const std::exception& e) {
                Log::Error("[ClientConnection] Exception applying packet: %s", e.what());
            }
        }
    }

} // namespace Client