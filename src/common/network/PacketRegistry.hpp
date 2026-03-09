// File: src/common/network/PacketRegistry.hpp
#pragma once

#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include <string>

    namespace Network {

        // Packet IDs (single byte, supports 256 packet types)
        enum class PacketId : uint8_t {
            // ========================================================================
            // HANDSHAKE PACKETS (0x00-0x0F)
            // ========================================================================
            Handshake           = 0x00,
            StatusRequest       = 0x01,
            StatusResponse      = 0x02,
            Ping                = 0x03,
            Pong                = 0x04,
            LoginStart          = 0x05,
            LoginSuccess        = 0x06,
            Disconnect          = 0x07,
            SetCompression      = 0x08,

            // ========================================================================
            // SERVER → CLIENT PACKETS (0x10-0x7F)
            // ========================================================================
            // 0x10 and 0x11 reserved (previously ServerChunkData/ServerChunkUnload)
            BlockChangeS2C      = 0x12,
            MultiBlockChangeS2C = 0x13,
            PlayerUpdateS2C     = 0x14,
            EntitySpawn         = 0x15,
            EntityMove          = 0x16,
            EntityDestroy       = 0x17,
            ChatMessageS2C      = 0x18,
            TimeUpdate          = 0x19,
            WeatherChange       = 0x1A,
            KeepAliveS2C        = 0x1B,
            PlayerListUpdate    = 0x1C,
            PlayerAbilities     = 0x1D,
            WorldSpawn          = 0x1E,
            ClientboundBlockUpdate = 0x1F,  // Minecraft-style block update packet
            ChunkDataS2C        = 0x20,  // New Minecraft-compatible chunk data packet
            UnloadChunkS2C      = 0x21,  // New Minecraft-compatible chunk unload packet
            ClientboundSectionBlocksUpdate = 0x22,  // Minecraft-style section block updates packet
            ChunkBatchStartS2C    = 0x23,  // Signals start of a chunk batch
            ChunkBatchFinishedS2C = 0x24,  // Signals end of batch (includes chunk count)
            HotbarSyncS2C         = 0x25,  // Server-authoritative hotbar contents
            SetChunkCacheRadiusS2C = 0x26, // Server tells client the effective view distance

            // ========================================================================
            // CLIENT → SERVER PACKETS (0x3E, 0x80-0xFF)
            // ========================================================================
            UseItemOnC2S        = 0x3E,  // Use item on block packet (try use, then place)
            BlockActionC2S      = 0x80,
            PlayerMoveC2S       = 0x81,
            ChatMessageC2S      = 0x82,
            ClientConfigC2S     = 0x83,
            UseItem             = 0x84,
            PlayerAction        = 0x85,
            KeepAliveC2S        = 0x86,
            PlayerRotation      = 0x87,
            PlayerPosition      = 0x88,
            PlayerPosRot        = 0x89,
            CloseWindow         = 0x8A,
            ClickWindow         = 0x8B,
            HeldItemChange      = 0x8C,
            Animation           = 0x8D,
            EntityAction        = 0x8E,
            SteerVehicle        = 0x8F,
            ChunkBatchAckC2S    = 0x90,  // Client acknowledges batch with desired send rate
        };

        // Convert PacketId to string for logging
        inline const char* PacketIdToString(PacketId id) {
            switch (id) {
                // Handshake
                case PacketId::Handshake: return "Handshake";
                case PacketId::StatusRequest: return "StatusRequest";
                case PacketId::StatusResponse: return "StatusResponse";
                case PacketId::Ping: return "Ping";
                case PacketId::Pong: return "Pong";
                case PacketId::LoginStart: return "LoginStart";
                case PacketId::LoginSuccess: return "LoginSuccess";
                case PacketId::Disconnect: return "Disconnect";
                case PacketId::SetCompression: return "SetCompression";

                // Server → Client
                case PacketId::BlockChangeS2C: return "BlockChangeS2C";
                case PacketId::ClientboundBlockUpdate: return "ClientboundBlockUpdate";
                case PacketId::MultiBlockChangeS2C: return "MultiBlockChangeS2C";
                case PacketId::PlayerUpdateS2C: return "PlayerUpdateS2C";
                case PacketId::EntitySpawn: return "EntitySpawn";
                case PacketId::EntityMove: return "EntityMove";
                case PacketId::EntityDestroy: return "EntityDestroy";
                case PacketId::ChatMessageS2C: return "ChatMessageS2C";
                case PacketId::TimeUpdate: return "TimeUpdate";
                case PacketId::WeatherChange: return "WeatherChange";
                case PacketId::KeepAliveS2C: return "KeepAliveS2C";
                case PacketId::PlayerListUpdate: return "PlayerListUpdate";
                case PacketId::PlayerAbilities: return "PlayerAbilities";
                case PacketId::WorldSpawn: return "WorldSpawn";
                case PacketId::ChunkDataS2C: return "ChunkDataS2C";
                case PacketId::UnloadChunkS2C: return "UnloadChunkS2C";
                case PacketId::ClientboundSectionBlocksUpdate: return "ClientboundSectionBlocksUpdate";
                case PacketId::ChunkBatchStartS2C: return "ChunkBatchStartS2C";
                case PacketId::ChunkBatchFinishedS2C: return "ChunkBatchFinishedS2C";
                case PacketId::HotbarSyncS2C: return "HotbarSyncS2C";

                // Client → Server
                case PacketId::UseItemOnC2S: return "UseItemOnC2S";
                case PacketId::BlockActionC2S: return "BlockActionC2S";
                case PacketId::PlayerMoveC2S: return "PlayerMoveC2S";
                case PacketId::ChatMessageC2S: return "ChatMessageC2S";
                case PacketId::ClientConfigC2S: return "ClientConfigC2S";
                case PacketId::UseItem: return "UseItem";
                case PacketId::PlayerAction: return "PlayerAction";
                case PacketId::KeepAliveC2S: return "KeepAliveC2S";
                case PacketId::PlayerRotation: return "PlayerRotation";
                case PacketId::PlayerPosition: return "PlayerPosition";
                case PacketId::PlayerPosRot: return "PlayerPosRot";
                case PacketId::CloseWindow: return "CloseWindow";
                case PacketId::ClickWindow: return "ClickWindow";
                case PacketId::HeldItemChange: return "HeldItemChange";
                case PacketId::Animation: return "Animation";
                case PacketId::EntityAction: return "EntityAction";
                case PacketId::SteerVehicle: return "SteerVehicle";
                case PacketId::ChunkBatchAckC2S: return "ChunkBatchAckC2S";

                default: return "Unknown";
            }
        }

        // ========================================================================
        // VARINT ENCODING/DECODING HELPERS
        // ========================================================================

        class VarInt {
        public:
            // Encode a 32-bit integer as VarInt
            static void Encode(uint32_t value, std::vector<uint8_t>& buffer) {
                while ((value & 0xFFFFFF80) != 0) {
                    buffer.push_back((value & 0x7F) | 0x80);
                    value >>= 7;
                }
                buffer.push_back(value & 0x7F);
            }

            // Encode a signed 32-bit integer as VarInt (ZigZag encoding)
            static void EncodeSigned(int32_t value, std::vector<uint8_t>& buffer) {
                uint32_t encoded = (value << 1) ^ (value >> 31);
                Encode(encoded, buffer);
            }

            // Decode VarInt from buffer
            static uint32_t Decode(const uint8_t* data, size_t& bytesRead) {
                uint32_t value = 0;
                size_t position = 0;
                uint8_t currentByte;

                bytesRead = 0;
                do {
                    if (bytesRead >= 5) {
                        throw std::runtime_error("VarInt too big");
                    }

                    currentByte = data[bytesRead];
                    value |= (currentByte & 0x7F) << position;

                    bytesRead++;
                    position += 7;
                } while ((currentByte & 0x80) != 0);

                return value;
            }

            // Decode signed VarInt (ZigZag decoding)
            static int32_t DecodeSigned(const uint8_t* data, size_t& bytesRead) {
                uint32_t encoded = Decode(data, bytesRead);
                return (encoded >> 1) ^ -(encoded & 1);
            }

            // Get the size in bytes a VarInt would take
            static size_t GetSize(uint32_t value) {
                size_t size = 0;
                while ((value & 0xFFFFFF80) != 0) {
                    size++;
                    value >>= 7;
                }
                return size + 1;
            }
        };

        // VarLong encoding (for 64-bit values)
        class VarLong {
        public:
            static void Encode(uint64_t value, std::vector<uint8_t>& buffer) {
                while ((value & 0xFFFFFFFFFFFFFF80ULL) != 0) {
                    buffer.push_back((value & 0x7F) | 0x80);
                    value >>= 7;
                }
                buffer.push_back(value & 0x7F);
            }

            static uint64_t Decode(const uint8_t* data, size_t& bytesRead) {
                uint64_t value = 0;
                size_t position = 0;
                uint8_t currentByte;

                bytesRead = 0;
                do {
                    if (bytesRead >= 10) {
                        throw std::runtime_error("VarLong too big");
                    }

                    currentByte = data[bytesRead];
                    value |= (static_cast<uint64_t>(currentByte & 0x7F)) << position;

                    bytesRead++;
                    position += 7;
                } while ((currentByte & 0x80) != 0);

                return value;
            }
        };

        // ========================================================================
        // PACKET REGISTRY
        // ========================================================================

        class PacketRegistry {
        public:
            // Packet handler function type
            using PacketHandler = std::function<void(const std::vector<uint8_t>&)>;

            // Register a packet handler
            void RegisterHandler(PacketId id, PacketHandler handler) {
                m_handlers[static_cast<uint8_t>(id)] = handler;
            }

            // Unregister a packet handler
            void UnregisterHandler(PacketId id) {
                m_handlers.erase(static_cast<uint8_t>(id));
            }

            // Handle a packet
            bool HandlePacket(uint8_t packetId, const std::vector<uint8_t>& payload) {
                auto it = m_handlers.find(packetId);
                if (it != m_handlers.end() && it->second) {
                    it->second(payload);
                    return true;
                }
                return false;
            }

            // Check if handler exists
            bool HasHandler(PacketId id) const {
                return m_handlers.find(static_cast<uint8_t>(id)) != m_handlers.end();
            }

            // Clear all handlers
            void ClearHandlers() {
                m_handlers.clear();
            }

        private:
            std::unordered_map<uint8_t, PacketHandler> m_handlers;
        };

        // ========================================================================
        // PACKET BUFFER HELPERS
        // PACKET BUFFER HELPERS
        // ========================================================================

        class PacketBuffer {
        public:
            explicit PacketBuffer(size_t initialCapacity = 256) {
                m_data.reserve(initialCapacity);
            }

            // Write methods
            void WriteByte(uint8_t value) {
                m_data.push_back(value);
            }

            void WriteShort(uint16_t value) {
                WriteByte((value >> 8) & 0xFF);
                WriteByte(value & 0xFF);
            }

            void WriteInt(uint32_t value) {
                WriteByte((value >> 24) & 0xFF);
                WriteByte((value >> 16) & 0xFF);
                WriteByte((value >> 8) & 0xFF);
                WriteByte(value & 0xFF);
            }

            void WriteLong(uint64_t value) {
                WriteInt((value >> 32) & 0xFFFFFFFF);
                WriteInt(value & 0xFFFFFFFF);
            }

            void WriteVarInt(uint32_t value) {
                VarInt::Encode(value, m_data);
            }

            void WriteVarLong(uint64_t value) {
                VarLong::Encode(value, m_data);
            }

            void WriteFloat(float value) {
                union { float f; uint32_t i; } conv;
                conv.f = value;
                WriteInt(conv.i);
            }

            void WriteDouble(double value) {
                union { double d; uint64_t l; } conv;
                conv.d = value;
                WriteLong(conv.l);
            }

            void WriteString(const std::string& str) {
                WriteVarInt(static_cast<uint32_t>(str.length()));
                m_data.insert(m_data.end(), str.begin(), str.end());
            }

            void WriteBytes(const uint8_t* data, size_t length) {
                m_data.insert(m_data.end(), data, data + length);
            }

            void WriteBytes(const std::vector<uint8_t>& data) {
                m_data.insert(m_data.end(), data.begin(), data.end());
            }

            // Get the buffer
            const std::vector<uint8_t>& GetData() const { return m_data; }
            std::vector<uint8_t>& GetData() { return m_data; }

            // Clear the buffer
            void Clear() { m_data.clear(); }

            // Get size
            size_t Size() const { return m_data.size(); }

        private:
            std::vector<uint8_t> m_data;
        };

        class PacketReader {
        public:
            explicit PacketReader(const std::vector<uint8_t>& data)
                : m_data(data), m_pos(0) {}

            // Read methods
            uint8_t ReadByte() {
                if (m_pos >= m_data.size()) {
                    throw std::runtime_error("PacketReader: out of bounds");
                }
                return m_data[m_pos++];
            }

            uint16_t ReadShort() {
                uint16_t value = (ReadByte() << 8);
                value |= ReadByte();
                return value;
            }

            uint32_t ReadInt() {
                uint32_t value = (ReadByte() << 24);
                value |= (ReadByte() << 16);
                value |= (ReadByte() << 8);
                value |= ReadByte();
                return value;
            }

            uint64_t ReadLong() {
                uint64_t value = (static_cast<uint64_t>(ReadInt()) << 32);
                value |= ReadInt();
                return value;
            }

            uint32_t ReadVarInt() {
                size_t bytesRead = 0;
                uint32_t value = VarInt::Decode(m_data.data() + m_pos, bytesRead);
                m_pos += bytesRead;
                return value;
            }

            int32_t ReadSignedVarInt() {
                size_t bytesRead = 0;
                int32_t value = VarInt::DecodeSigned(m_data.data() + m_pos, bytesRead);
                m_pos += bytesRead;
                return value;
            }

            uint64_t ReadVarLong() {
                size_t bytesRead = 0;
                uint64_t value = VarLong::Decode(m_data.data() + m_pos, bytesRead);
                m_pos += bytesRead;
                return value;
            }

            float ReadFloat() {
                union { float f; uint32_t i; } conv;
                conv.i = ReadInt();
                return conv.f;
            }

            double ReadDouble() {
                union { double d; uint64_t l; } conv;
                conv.l = ReadLong();
                return conv.d;
            }

            std::string ReadString() {
                uint32_t length = ReadVarInt();
                if (m_pos + length > m_data.size()) {
                    throw std::runtime_error("PacketReader: string out of bounds");
                }
                std::string str(m_data.begin() + m_pos, m_data.begin() + m_pos + length);
                m_pos += length;
                return str;
            }

            // Read string with maximum length validation
            std::string ReadString(size_t maxLength) {
                uint32_t length = ReadVarInt();
                if (length > maxLength) {
                    throw std::runtime_error("PacketReader: string length " + std::to_string(length) + 
                                           " exceeds maximum " + std::to_string(maxLength));
                }
                if (m_pos + length > m_data.size()) {
                    throw std::runtime_error("PacketReader: string out of bounds");
                }
                std::string str(m_data.begin() + m_pos, m_data.begin() + m_pos + length);
                m_pos += length;
                return str;
            }

            void ReadBytes(uint8_t* buffer, size_t length) {
                if (m_pos + length > m_data.size()) {
                    throw std::runtime_error("PacketReader: bytes out of bounds");
                }
                std::memcpy(buffer, m_data.data() + m_pos, length);
                m_pos += length;
            }

            std::vector<uint8_t> ReadBytes(size_t length) {
                if (m_pos + length > m_data.size()) {
                    throw std::runtime_error("PacketReader: bytes out of bounds");
                }
                std::vector<uint8_t> result(m_data.begin() + m_pos, m_data.begin() + m_pos + length);
                m_pos += length;
                return result;
            }

            // Check if more data available
            bool HasMore() const { return m_pos < m_data.size(); }

            // Get remaining bytes
            size_t Remaining() const { return m_data.size() - m_pos; }

            // Get current position
            size_t Position() const { return m_pos; }

            // Skip bytes
            void Skip(size_t bytes) {
                m_pos = std::min(m_pos + bytes, m_data.size());
            }

        private:
            const std::vector<uint8_t>& m_data;
            size_t m_pos;
        };
    }
// namespace Network