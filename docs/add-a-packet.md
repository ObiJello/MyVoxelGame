Runbook: Adding a New Packet

This guide walks through adding a new packet type to the networking system, from initial definition to testing and documentation.

Overview Steps

1. Define packet structure and assign ID
2. Add to packet registry and serialization
3. Create listener method and implement handler
4. Add encoding/decoding logic
5. Update documentation and test

Step 1: Define Packet Structure

Choose Packet ID

Assign an unused packet ID following our local allocation scheme:
- 0x00-0x0F: Handshake/setup packets
- 0x10-0x7F: Server→Client packets  
- 0x80-0xFF: Client→Server packets

Note: Minecraft uses VarInt IDs that are state-scoped (LOGIN/PLAY/etc.)—ranges shown are our local convention for simplified implementation. For true protocol fidelity, IDs should be VarInt and state-scoped.


    // In src/common/network/PacketRegistry.hpp
    enum class PacketId : uint8_t {
        // ... existing packets
        WeatherChangeS2C = 0x1F,  // New server→client packet
        PlayerSettingsC2S = 0x8F, // New client→server packet
    };

Define Packet Struct

Create the packet data structure in src/common/network/PacketTypes.hpp:

    // Server→Client weather change packet
    struct WeatherChangeS2CPacket {
        enum class WeatherType : uint8_t {
            CLEAR = 0,
            RAIN = 1,
            THUNDERSTORM = 2
        };

        WeatherType weatherType;
        float intensity;        // 0.0-1.0
        uint32_t duration;      // Duration in ticks

        // Constructor for easy creation
        WeatherChangeS2CPacket(WeatherType type, float intensity, uint32_t duration)
          : weatherType(type), intensity(intensity), duration(duration) {}
        };
    
        // Client→Server player settings packet  
        struct PlayerSettingsC2SPacket {
        std::string locale;             // e.g., "en_US"
        uint8_t viewDistance;           // 2-32 chunks
        uint8_t chatMode;              // 0=enabled, 1=commands only, 2=hidden
        bool chatColors;               // Enable chat colors
        uint8_t displayedSkinParts;    // Bitmask of skin parts
        uint8_t mainHand;              // 0=left, 1=right

        PlayerSettingsC2SPacket() = default;
    };

Step 2: Update Packet Registry

Add to PacketIdToString()

    // In src/common/network/PacketRegistry.hpp:71-123
    const char* PacketIdToString(PacketId id) {
        switch (id) {
            // ... existing cases
            case PacketId::WeatherChangeS2C: return "WeatherChangeS2C";
            case PacketId::PlayerSettingsC2S: return "PlayerSettingsC2S";
            // ...
        }
    }

Add Serialization Support

    // In packet serialization system
    namespace Network::Serialization {

        // Serialize WeatherChangeS2CPacket
        template<>
        std::vector<uint8_t> Serialize(const WeatherChangeS2CPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteByte(static_cast<uint8_t>(packet.weatherType));
            buffer.WriteFloat(packet.intensity);
            buffer.WriteInt(packet.duration);
            return buffer.GetData();
        }

        // Deserialize WeatherChangeS2CPacket  
        template<>
        WeatherChangeS2CPacket Deserialize(const std::vector<uint8_t>& data) {
            PacketReader reader(data);

            WeatherChangeS2CPacket packet;
            packet.weatherType = static_cast<WeatherChangeS2CPacket::WeatherType>(reader.ReadByte());
            packet.intensity = reader.ReadFloat();
            packet.duration = reader.ReadInt();
            return packet;
        }
    }

Step 3: Add Listener Methods

Server-Side Listener (for C2S packets)

    // In src/server/network/listeners/IServerPlayPacketListener.hpp
    class IServerPlayPacketListener {
        public:
            // ... existing methods
            virtual void onPlayerSettings(const PlayerSettingsC2SPacket& packet) = 0;
    };
    
    // In src/server/network/listeners/PlayPacketListener.hpp  
    class PlayPacketListener : public IServerPlayPacketListener {
        public:
            // ... existing methods
            void onPlayerSettings(const PlayerSettingsC2SPacket& packet) override;
    };

Client-Side Handler (for S2C packets)

    // In src/client/network/ClientPacketHandler.hpp
    class ClientPacketHandler {
        public:
            // ... existing methods
            void HandleWeatherChange(const WeatherChangeS2CPacket& packet);
    };

Step 4: Implement Packet Handlers

Server-Side Implementation

    // In src/server/network/listeners/PlayPacketListener.cpp
    void PlayPacketListener::onPlayerSettings(const PlayerSettingsC2SPacket& packet) {
        Log::Info("Player settings: locale=%s, viewDistance=%d, chatMode=%d",
            packet.locale.c_str(), packet.viewDistance, packet.chatMode);

        // Validate settings
        if (packet.viewDistance < 2 || packet.viewDistance > 32) {
            Log::Warning("Invalid view distance: %d", packet.viewDistance);
            m_connection.SendDisconnect("Invalid client settings");
            return;
        }

        // Apply settings to player session
        auto* server = m_connection.GetServer();
        if (server) {
            // Update player's view distance, chat settings, etc.
            server->UpdatePlayerSettings(m_connection.GetPlayerId(), packet);
        }
    }

Client-Side Implementation

    // In src/client/network/ClientPacketHandler.cpp
    void ClientPacketHandler::HandleWeatherChange(const WeatherChangeS2CPacket& packet) {
        Log::Info("Weather changed to %s, intensity=%.2f, duration=%d ticks",
        GetWeatherTypeName(packet.weatherType),
        packet.intensity,
        packet.duration);
    
        // Update weather rendering system
        auto* weatherSystem = GetWeatherSystem();
        if (weatherSystem) {
            weatherSystem->SetWeather(packet.weatherType, packet.intensity, packet.duration);
        }

        // Update sound system  
        auto* audioSystem = GetAudioSystem();
        if (audioSystem) {
            audioSystem->PlayWeatherSounds(packet.weatherType, packet.intensity);
        }
    }

Step 5: Add Encoding/Decoding Logic

Connection Decode Methods

    // In src/server/network/ServerConnection.cpp - DecodePlayPacket()
    std::unique_ptr<IPacket> ServerConnection::DecodePlayPacket(uint8_t packetId, PacketReader& reader) {
        switch (packetId) {
            // ... existing cases
            case static_cast<uint8_t>(PacketId::PlayerSettingsC2S): {
                auto packet = std::make_unique<PlayerSettingsC2SPacket>();
                packet->locale = reader.ReadString();
                packet->viewDistance = reader.ReadByte();
                packet->chatMode = reader.ReadByte();
                packet->chatColors = reader.ReadByte() != 0;
                packet->displayedSkinParts = reader.ReadByte();
                packet->mainHand = reader.ReadByte();
                return packet;
            }
            // ...
        }
    }

Packet Sending Methods

    // In src/server/network/ServerConnection.hpp
    class ServerConnection {
        public:
            // ... existing methods
            void SendWeatherChange(WeatherChangeS2CPacket::WeatherType type, float intensity, uint32_t duration);
    };
    
    // In src/server/network/ServerConnection.cpp
    void ServerConnection::SendWeatherChange(WeatherChangeS2CPacket::WeatherType type, float intensity, uint32_t duration) {
        WeatherChangeS2CPacket packet(type, intensity, duration);
        auto data = Network::Serialization::Serialize(packet);

        // Send via network
        EnqueueOutboundPacket(static_cast<uint8_t>(PacketId::WeatherChangeS2C), data);
    }

Step 6: Wire Up Packet Dispatch

Add to Packet Dispatch Table

    // In appropriate packet handler registration
    void RegisterPacketHandlers() {
        // Server-side (C2S packets)
        m_packetRegistry.RegisterHandler(PacketId::PlayerSettingsC2S,
            [this](const std::vector<uint8_t>& payload) {
            auto packet = Network::Serialization::Deserialize<PlayerSettingsC2SPacket>(payload);
            m_listener->onPlayerSettings(packet);
        });

        // Client-side (S2C packets)  
        m_packetRegistry.RegisterHandler(PacketId::WeatherChangeS2C,
          [this](const std::vector<uint8_t>& payload) {
              auto packet = Network::Serialization::Deserialize<WeatherChangeS2CPacket>(payload);
              m_clientHandler->HandleWeatherChange(packet);
          });
    }

Step 7: Testing

Unit Tests

    // Test packet serialization/deserialization
    TEST(PacketTest, WeatherChangeS2CRoundtrip) {
        WeatherChangeS2CPacket original(
            WeatherChangeS2CPacket::WeatherType::RAIN,
            0.75f,
            1200
        );

        auto serialized = Network::Serialization::Serialize(original);
        auto deserialized = Network::Serialization::Deserialize<WeatherChangeS2CPacket>(serialized);

        EXPECT_EQ(original.weatherType, deserialized.weatherType);
        EXPECT_FLOAT_EQ(original.intensity, deserialized.intensity);
        EXPECT_EQ(original.duration, deserialized.duration);
    }   

Integration Testing

    // Test full packet flow in development environment
    void TestWeatherPacket() {
        // Server: Send weather change
        auto* connection = GetTestServerConnection();
        connection->SendWeatherChange(WeatherChangeS2CPacket::WeatherType::THUNDERSTORM, 1.0f, 2400);

        // Client: Verify packet received and processed
        auto* client = GetTestClient();
        client->ProcessPendingPackets();

        auto* weather = client->GetWeatherSystem();
        ASSERT_NE(weather, nullptr);
        EXPECT_EQ(weather->GetCurrentWeather(), WeatherType::THUNDERSTORM);
        EXPECT_FLOAT_EQ(weather->GetIntensity(), 1.0f);
    }

Step 8: Update Documentation

Update Packet Registry Table

Add entries to docs/protocol/packet-registry.md:

| ID   | Direction | State | Name              | Listener                                    | Payload
|
|------|-----------|-------|-------------------|---------------------------------------------|-------------------------------------------------------------------------------------------------------
---|
| 0x1F | S2C       | PLAY  | WeatherChangeS2C  | Client                                      | Byte(weatherType), Float(intensity), Int(duration)
|
| 0x8F | C2S       | PLAY  | PlayerSettingsC2S | IServerPlayPacketListener::onPlayerSettings | String(locale), Byte(viewDistance), Byte(chatMode), Boolean(chatColors), Byte(skinParts),
Byte(mainHand) |

Update Protocol Documentation

Add to relevant protocol flow diagrams and integration documentation.

Common Issues and Solutions

Packet ID Conflicts

Problem: Packet ID already in use
Solution: Check PacketRegistry.hpp for available IDs, update documentation

Serialization Size Mismatch

Problem: Deserialize fails with "out of bounds" error
Solution: Ensure serialize/deserialize methods match field order and types exactly

Listener Not Called

Problem: Packet handler never executesSolution: Verify packet dispatch registration and protocol state validity

Compression Issues

Problem: Large packets fail after compression enabled
Solution: Test packet with various payload sizes, ensure compression threshold handled correctly

Performance Considerations

Packet Size Optimization

- Use VarInt instead of Int for values typically <128
- Pack boolean flags into single byte bitfields
- Consider string length limits for user input

Frequency Limits

- Add rate limiting for frequently sent packets
- Batch multiple changes into single packet when possible
- Consider delta compression for repeated similar data

Validation Performance

- Validate packet data efficiently in handlers
- Use early returns for invalid data
- Log validation failures for debugging but don't spam

This runbook provides a complete workflow for safely adding new packet types while maintaining protocol compatibility and system reliability.
