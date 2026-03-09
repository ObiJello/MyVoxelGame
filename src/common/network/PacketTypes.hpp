// File: src/common/network/PacketTypes.hpp
#pragma once

#include "../world/math/WorldMath.hpp"
#include "../world/math/WorldCoordinates.hpp"
#include "../world/block/Blocks.hpp"
#include "../world/chunk/Chunk.hpp"
#include "PacketRegistry.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <chrono>

namespace Network {

    // Forward declarations
    struct SerializedChunkData;

    // ========================================================================
    // SERVER → CLIENT PACKETS
    // ========================================================================

    // Minecraft-compatible chunk data packet format (protocol 0x20)
    struct ChunkDataS2CPacket {
        // Chunk coordinates
        int32_t chunkX;
        int32_t chunkZ;
        
        // If true, send all sections plus biomes; otherwise only sections in bitmask
        bool groundUpContinuous = true;
        
        // Bitmask indicating which sections contain data (VarInt)
        // Bit 0 = section at minBuildHeight, bit 1 = next section up, etc.
        uint32_t primaryBitmask = 0;
        
        // Palletted container data for each section
        struct SectionData {
            uint16_t blockCount = 0;  // Number of non-air blocks
            uint8_t bitsPerEntry = 0;  // Bits per block index (for palette mode)
            std::vector<uint32_t> palette;  // Block state IDs in palette (optional)
            std::vector<uint64_t> dataArray;  // Packed block indices/IDs
            
            bool IsEmpty() const { return blockCount == 0; }
        };
        
        // Section data for each section with bit set in primaryBitmask
        std::vector<SectionData> sections;
        
        // Optional biome data (256 bytes for ground-up continuous)
        std::vector<uint8_t> biomeData;
        
        // Timestamp for tracking
        std::chrono::steady_clock::time_point timestamp;
        
        ChunkDataS2CPacket() = default;
        ChunkDataS2CPacket(int32_t x, int32_t z) 
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}
        
        // Calculate total data size for the packet
        size_t CalculateDataSize() const {
            size_t size = 0;
            for (const auto& section : sections) {
                size += sizeof(section.blockCount) + sizeof(section.bitsPerEntry);
                size += section.palette.size() * sizeof(uint32_t);
                size += section.dataArray.size() * sizeof(uint64_t);
            }
            size += biomeData.size();
            return size;
        }
    };

    // Minecraft-compatible chunk unload packet (protocol 0x21)
    struct UnloadChunkS2CPacket {
        int32_t chunkX;
        int32_t chunkZ;
        std::chrono::steady_clock::time_point timestamp;
        
        UnloadChunkS2CPacket() = default;
        UnloadChunkS2CPacket(int32_t x, int32_t z)
            : chunkX(x), chunkZ(z), timestamp(std::chrono::steady_clock::now()) {}
    };

    // Chunk batch start marker (empty, like Minecraft's ClientboundChunkBatchStartPacket.INSTANCE)
    struct ChunkBatchStartS2CPacket {};

    // Chunk batch finished (tells client how many chunks were in this batch)
    struct ChunkBatchFinishedS2CPacket {
        int32_t batchSize = 0;
        ChunkBatchFinishedS2CPacket() = default;
        explicit ChunkBatchFinishedS2CPacket(int32_t size) : batchSize(size) {}
    };

    // Server tells client the effective view distance (like Minecraft's ClientboundSetChunkCacheRadiusPacket)
    struct SetChunkCacheRadiusS2CPacket {
        int32_t viewDistance = 8;
        SetChunkCacheRadiusS2CPacket() = default;
        explicit SetChunkCacheRadiusS2CPacket(int32_t dist) : viewDistance(dist) {}
    };

    // Client acknowledges chunk batch with desired send rate
    struct ChunkBatchAckC2SPacket {
        float desiredChunksPerTick = 9.0f;
        ChunkBatchAckC2SPacket() = default;
        explicit ChunkBatchAckC2SPacket(float rate) : desiredChunksPerTick(rate) {}
    };

    // Individual block changes broadcast by server (mirrors Minecraft's BlockUpdateS2CPacket)
    struct BlockChangeS2CPacket {
        int worldX, worldY, worldZ;  // Use individual coordinates instead of WorldCoordinates
        Game::BlockID newBlockId;
        uint32_t timestamp;
        bool playSound = true;
        bool updateNeighbors = true;

        BlockChangeS2CPacket() = default;
        BlockChangeS2CPacket(int x, int y, int z, Game::BlockID blockId)
            : worldX(x), worldY(y), worldZ(z), newBlockId(blockId)
            , timestamp(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {}
    };

    // Minecraft-style clientbound block update packet (modern naming convention)
    struct ClientboundBlockUpdateS2CPacket {
        int32_t x, y, z;        // Block position
        int32_t blockId;        // Block ID as int (not BlockID enum for flexibility)
        
        ClientboundBlockUpdateS2CPacket() = default;
        ClientboundBlockUpdateS2CPacket(int32_t x, int32_t y, int32_t z, int32_t blockId)
            : x(x), y(y), z(z), blockId(blockId) {}
    };

    // Multiple block changes in one packet for efficiency
    struct MultiBlockChangeS2CPacket {
        Game::Math::ChunkPos chunkPos;
        struct BlockChange {
            uint8_t localX, localY, localZ; // 0-15 within chunk
            Game::BlockID blockId;
        };
        std::vector<BlockChange> changes;
        uint32_t timestamp;
    };
    
    // Minecraft-compatible section block updates packet (1.16.2+ format)
    // All changes MUST be within the same chunk section (16x16x16)
    struct ClientboundSectionBlocksUpdateS2CPacket {
        Game::Math::ChunkPos chunkPos;  // Chunk coordinates
        int32_t sectionY;                // Section index (0-23 for -64 to 319 world height)
        
        // Packed block changes following Minecraft wire format:
        // Each VarInt encodes: (localX << 12) | (localZ << 8) | (localY << 4) | blockStateId
        // localX, localY, localZ are 0-15 within the section
        // blockStateId uses remaining bits (can extend beyond 4 bits if needed)
        std::vector<uint32_t> packedRecords;
        
        ClientboundSectionBlocksUpdateS2CPacket() = default;
        ClientboundSectionBlocksUpdateS2CPacket(Game::Math::ChunkPos pos, int32_t section)
            : chunkPos(pos), sectionY(section) {}
        
        // Helper to add a block change
        void AddChange(uint8_t localX, uint8_t localY, uint8_t localZ, uint16_t blockStateId) {
            // Pack according to Minecraft protocol
            uint32_t packed = (static_cast<uint32_t>(localX & 0xF) << 12) |
                            (static_cast<uint32_t>(localZ & 0xF) << 8) |
                            (static_cast<uint32_t>(localY & 0xF) << 4) |
                            (blockStateId & 0xFFF);  // Support up to 12 bits for block ID
            packedRecords.push_back(packed);
        }
        
        // Helper to unpack a record
        static void UnpackRecord(uint32_t packed, uint8_t& localX, uint8_t& localY, 
                                uint8_t& localZ, uint16_t& blockStateId) {
            localX = (packed >> 12) & 0xF;
            localZ = (packed >> 8) & 0xF;
            localY = (packed >> 4) & 0xF;
            blockStateId = packed & 0xFFF;
        }
    };

    // Server-authoritative hotbar sync (sent on join)
    struct HotbarSyncS2CPacket {
        std::array<uint16_t, 9> slots;
        HotbarSyncS2CPacket() { slots.fill(0); }
    };

    // Player position updates from server (for multiplayer support later)
    struct PlayerUpdateS2CPacket {
        uint32_t playerId;
        glm::vec3 position;
        glm::vec2 rotation;
        uint32_t sequenceNumber;
    };

    // ========================================================================
    // CLIENT → SERVER PACKETS  
    // ========================================================================

    enum class BlockActionType : uint8_t {
        BREAK = 0,
        PLACE = 1,
        INTERACT = 2
    };

    // Block interactions sent from client to server (mirrors Minecraft's PlayerActionC2SPacket)
    struct BlockActionC2SPacket {
        int worldX, worldY, worldZ;  // Use individual coordinates instead of WorldCoordinates
        BlockActionType action;
        Game::BlockID blockId = Game::BlockID::Air; // For PLACE action
        uint8_t face = 0; // Face being interacted with (0-5)
        glm::vec3 hitPosition; // Exact hit position for placement
        uint32_t sequenceNumber = 0;

        BlockActionC2SPacket() = default;
        BlockActionC2SPacket(int x, int y, int z, BlockActionType act)
            : worldX(x), worldY(y), worldZ(z), action(act) {}
        BlockActionC2SPacket(int x, int y, int z, BlockActionType act, Game::BlockID block)
            : worldX(x), worldY(y), worldZ(z), action(act), blockId(block) {}
    };

    // Use item on block packet sent from client to server (Minecraft 1.14+ style)
    // This packet first tries block.use(), then item placement
    struct UseItemOnC2SPacket {
        uint32_t hand = 0;           // VarInt: 0=main hand, 1=off hand
        int32_t blockX = 0;          // Block coordinates of the block clicked on
        int32_t blockY = 0;
        int32_t blockZ = 0;
        uint32_t direction = 0;      // VarInt: Face clicked (0=bottom, 1=top, 2=north, 3=south, 4=west, 5=east)
        float cursorX = 0.0f;        // Hit position X in block-local coordinates [0,1)
        float cursorY = 0.0f;        // Hit position Y in block-local coordinates [0,1)
        float cursorZ = 0.0f;        // Hit position Z in block-local coordinates [0,1)
        bool insideBlock = false;    // True if raycast started inside the block volume
        uint32_t sequence = 0;       // VarInt: Monotonic client interaction ID for acknowledgment

        UseItemOnC2SPacket() = default;
        UseItemOnC2SPacket(uint32_t h, int32_t x, int32_t y, int32_t z, uint32_t dir,
                           float cx, float cy, float cz, bool inside, uint32_t seq)
            : hand(h), blockX(x), blockY(y), blockZ(z), direction(dir),
              cursorX(cx), cursorY(cy), cursorZ(cz), insideBlock(inside), sequence(seq) {}
    };

    // Player movement sent from client to server (mirrors Minecraft's PlayerMoveC2SPacket)
    struct PlayerMoveC2SPacket {
        glm::vec3 position;
        glm::vec2 rotation; // yaw, pitch
        bool onGround = false;
        uint32_t sequenceNumber = 0;
        std::chrono::steady_clock::time_point timestamp;

        PlayerMoveC2SPacket() = default;
        PlayerMoveC2SPacket(const glm::vec3& pos, const glm::vec2& rot)
            : position(pos), rotation(rot), timestamp(std::chrono::steady_clock::now()) {}
    };

    // Held item (hotbar slot) change from client to server
    struct HeldItemChangeC2SPacket {
        int16_t slot = 0;
        HeldItemChangeC2SPacket() = default;
        HeldItemChangeC2SPacket(int16_t s) : slot(s) {}
    };

    // Chat messages and commands
    struct ChatMessageC2SPacket {
        std::string message;
        uint32_t timestamp;
        bool isCommand = false; // True if message starts with '/'
    };

    // Client configuration and settings
    struct ClientConfigC2SPacket {
        int renderDistance = 8;
        bool enableVSync = true;
        float mouseSensitivity = 1.0f;
    };

    // ========================================================================
    // WORKER THREAD RESULT PACKETS
    // ========================================================================

    // Chunk generation results (Server worker → Server thread)
    struct ChunkGenResult {
        Game::Math::ChunkPos position;
        std::shared_ptr<Game::Chunk> chunk;
        bool success = false;
        std::chrono::steady_clock::time_point completeTime;
        std::string errorMessage;

        ChunkGenResult() = default;
        ChunkGenResult(Game::Math::ChunkPos pos, std::shared_ptr<Game::Chunk> chunkPtr, bool succeeded)
            : position(pos), chunk(std::move(chunkPtr)), success(succeeded)
            , completeTime(std::chrono::steady_clock::now()) {}
    };

    // Mesh build results (Client worker → Client render thread)
    struct MeshBuildResult {
        Game::Math::ChunkPos chunkPos;
        int sectionY;
        uint32_t generation = 0;  // Version number for staleness checking
        uint8_t neighborMask = 0;  // Neighbor presence mask (PX=1, NX=2, PZ=4, NZ=8)
        struct SectionMeshData {
            // Opaque geometry (solid blocks like stone, dirt)
            std::vector<float> opaqueVertices;
            std::vector<uint32_t> opaqueIndices;
            
            // Cutout geometry (alpha-test blocks like leaves, grass)
            std::vector<float> cutoutVertices;
            std::vector<uint32_t> cutoutIndices;
            
            // Translucent geometry (blended blocks like glass, water, ice)
            std::vector<float> translucentVertices;
            std::vector<uint32_t> translucentIndices;
            
            // Layer counts for validation
            size_t opaqueVertexCount = 0;
            size_t opaqueIndexCount = 0;
            size_t cutoutVertexCount = 0;
            size_t cutoutIndexCount = 0;
            size_t translucentVertexCount = 0;
            size_t translucentIndexCount = 0;
            
            // Check if any layer has geometry
            bool IsEmpty() const {
                return opaqueVertices.empty() && cutoutVertices.empty() && translucentVertices.empty();
            }
            
            // Get total vertex count across all layers
            size_t GetTotalVertexCount() const {
                return opaqueVertexCount + cutoutVertexCount + translucentVertexCount;
            }
            
            // Get total index count across all layers
            size_t GetTotalIndexCount() const {
                return opaqueIndexCount + cutoutIndexCount + translucentIndexCount;
            }
        };
        SectionMeshData meshData;
        bool success = false;
        std::chrono::steady_clock::time_point completeTime;

        MeshBuildResult() = default;
        MeshBuildResult(Game::Math::ChunkPos pos, int secY)
            : chunkPos(pos), sectionY(secY), completeTime(std::chrono::steady_clock::now()) {}
    };

    // ========================================================================
    // SERIALIZATION SUPPORT
    // ========================================================================

    // Compressed chunk data for network transmission
    struct SerializedChunkData {
        std::vector<uint8_t> blockData;      // Compressed block IDs
        std::vector<uint8_t> lightData;      // Light data (optional)
        std::vector<uint8_t> biomeData;      // Biome data (optional)
        std::vector<uint8_t> heightmapData;  // Heightmap data (optional)
        
        uint32_t uncompressedSize = 0;
        uint32_t compressionType = 0; // 0 = none, 1 = zlib, etc.
        bool hasLightData = false;
        bool hasBiomeData = false;
        bool hasHeightmapData = false;

        // Calculate total serialized size
        size_t GetTotalSize() const {
            return blockData.size() + lightData.size() + biomeData.size() + heightmapData.size();
        }
    };

    // ========================================================================
    // PACKET UTILITY FUNCTIONS
    // ========================================================================

    // Packet size calculation for memory management
    template<typename PacketType>
    size_t GetPacketSize(const PacketType& packet);

    // Packet validation
    template<typename PacketType>
    bool ValidatePacket(const PacketType& packet);

    // Packet statistics for debugging
    struct PacketStats {
        size_t serverToClientCount = 0;
        size_t clientToServerCount = 0;
        size_t chunkGenResultCount = 0;
        size_t meshBuildResultCount = 0;
        size_t totalBytesTransferred = 0;

        void Reset() {
            serverToClientCount = clientToServerCount = 0;
            chunkGenResultCount = meshBuildResultCount = 0;
            totalBytesTransferred = 0;
        }
    };

    // ========================================================================
    // PACKET SERIALIZATION METHODS
    // ========================================================================

    namespace Serialization {

        // ---- BlockChangeS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const BlockChangeS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteShort(static_cast<uint16_t>(packet.newBlockId));
            buffer.WriteByte((packet.playSound ? 0x01 : 0) | (packet.updateNeighbors ? 0x02 : 0));
            return buffer.GetData();
        }
        
        inline BlockChangeS2CPacket DeserializeBlockChangeS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            BlockChangeS2CPacket packet;
            packet.worldX = reader.ReadInt();
            packet.worldY = reader.ReadInt();
            packet.worldZ = reader.ReadInt();
            packet.newBlockId = static_cast<Game::BlockID>(reader.ReadShort());
            uint8_t flags = reader.ReadByte();
            packet.playSound = (flags & 0x01) != 0;
            packet.updateNeighbors = (flags & 0x02) != 0;
            packet.timestamp = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            return packet;
        }

        // ---- ClientboundBlockUpdateS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const ClientboundBlockUpdateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.x);
            buffer.WriteInt(packet.y);
            buffer.WriteInt(packet.z);
            buffer.WriteInt(packet.blockId);
            return buffer.GetData();
        }
        
        inline ClientboundBlockUpdateS2CPacket DeserializeClientboundBlockUpdate(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ClientboundBlockUpdateS2CPacket packet;
            packet.x = reader.ReadInt();
            packet.y = reader.ReadInt();
            packet.z = reader.ReadInt();
            packet.blockId = reader.ReadInt();
            return packet;
        }

        // ---- MultiBlockChangeS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const MultiBlockChangeS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.chunkPos.x);
            buffer.WriteInt(packet.chunkPos.z);
            buffer.WriteVarInt(static_cast<uint32_t>(packet.changes.size()));
            for (const auto& change : packet.changes) {
                buffer.WriteByte(change.localX);
                buffer.WriteByte(change.localY);
                buffer.WriteByte(change.localZ);
                buffer.WriteShort(static_cast<uint16_t>(change.blockId));
            }
            buffer.WriteInt(packet.timestamp);
            return buffer.GetData();
        }
        
        inline MultiBlockChangeS2CPacket DeserializeMultiBlockChangeS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            MultiBlockChangeS2CPacket packet;
            packet.chunkPos.x = reader.ReadInt();
            packet.chunkPos.z = reader.ReadInt();
            uint32_t changeCount = reader.ReadVarInt();
            packet.changes.reserve(changeCount);
            for (uint32_t i = 0; i < changeCount; ++i) {
                MultiBlockChangeS2CPacket::BlockChange change;
                change.localX = reader.ReadByte();
                change.localY = reader.ReadByte();
                change.localZ = reader.ReadByte();
                change.blockId = static_cast<Game::BlockID>(reader.ReadShort());
                packet.changes.push_back(change);
            }
            packet.timestamp = reader.ReadInt();
            return packet;
        }
        
        // ---- PlayerUpdateS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const PlayerUpdateS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.playerId);
            buffer.WriteFloat(packet.position.x);
            buffer.WriteFloat(packet.position.y);
            buffer.WriteFloat(packet.position.z);
            buffer.WriteFloat(packet.rotation.x);
            buffer.WriteFloat(packet.rotation.y);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }
        
        inline PlayerUpdateS2CPacket DeserializePlayerUpdateS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            PlayerUpdateS2CPacket packet;
            packet.playerId = reader.ReadVarInt();
            packet.position.x = reader.ReadFloat();
            packet.position.y = reader.ReadFloat();
            packet.position.z = reader.ReadFloat();
            packet.rotation.x = reader.ReadFloat();
            packet.rotation.y = reader.ReadFloat();
            packet.sequenceNumber = reader.ReadVarInt();
            return packet;
        }

        // ---- BlockActionC2SPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const BlockActionC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.worldX);
            buffer.WriteInt(packet.worldY);
            buffer.WriteInt(packet.worldZ);
            buffer.WriteByte(static_cast<uint8_t>(packet.action));
            buffer.WriteShort(static_cast<uint16_t>(packet.blockId));
            buffer.WriteByte(packet.face);
            buffer.WriteFloat(packet.hitPosition.x);
            buffer.WriteFloat(packet.hitPosition.y);
            buffer.WriteFloat(packet.hitPosition.z);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }
        
        inline BlockActionC2SPacket DeserializeBlockActionC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            BlockActionC2SPacket packet;
            packet.worldX = reader.ReadInt();
            packet.worldY = reader.ReadInt();
            packet.worldZ = reader.ReadInt();
            packet.action = static_cast<BlockActionType>(reader.ReadByte());
            packet.blockId = static_cast<Game::BlockID>(reader.ReadShort());
            packet.face = reader.ReadByte();
            packet.hitPosition.x = reader.ReadFloat();
            packet.hitPosition.y = reader.ReadFloat();
            packet.hitPosition.z = reader.ReadFloat();
            packet.sequenceNumber = reader.ReadVarInt();
            return packet;
        }

        // ---- PlayerMoveC2SPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const PlayerMoveC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteDouble(packet.position.x);
            buffer.WriteDouble(packet.position.y);
            buffer.WriteDouble(packet.position.z);
            buffer.WriteFloat(packet.rotation.x); // yaw
            buffer.WriteFloat(packet.rotation.y); // pitch
            buffer.WriteByte(packet.onGround ? 0x01 : 0x00);
            buffer.WriteVarInt(packet.sequenceNumber);
            return buffer.GetData();
        }
        
        inline PlayerMoveC2SPacket DeserializePlayerMoveC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            PlayerMoveC2SPacket packet;
            packet.position.x = reader.ReadDouble();
            packet.position.y = reader.ReadDouble();
            packet.position.z = reader.ReadDouble();
            packet.rotation.x = reader.ReadFloat();
            packet.rotation.y = reader.ReadFloat();
            packet.onGround = reader.ReadByte() != 0;
            packet.sequenceNumber = reader.ReadVarInt();
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

        // ---- ChatMessageC2SPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const ChatMessageC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteString(packet.message);
            buffer.WriteByte(packet.isCommand ? 0x01 : 0x00);
            buffer.WriteInt(packet.timestamp);
            return buffer.GetData();
        }
        
        inline ChatMessageC2SPacket DeserializeChatMessageC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChatMessageC2SPacket packet;
            packet.message = reader.ReadString();
            packet.isCommand = reader.ReadByte() != 0;
            packet.timestamp = reader.ReadInt();
            return packet;
        }

        // ---- UseItemOnC2SPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const UseItemOnC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.hand);                // Hand (VarInt)
            buffer.WriteInt(packet.blockX);                 // Block X
            buffer.WriteInt(packet.blockY);                 // Block Y
            buffer.WriteInt(packet.blockZ);                 // Block Z
            buffer.WriteVarInt(packet.direction);           // Direction/Face (VarInt)
            buffer.WriteFloat(packet.cursorX);              // Cursor X [0,1)
            buffer.WriteFloat(packet.cursorY);              // Cursor Y [0,1)
            buffer.WriteFloat(packet.cursorZ);              // Cursor Z [0,1)
            buffer.WriteByte(packet.insideBlock ? 0x01 : 0x00); // Inside block flag
            buffer.WriteVarInt(packet.sequence);            // Sequence number (VarInt)
            return buffer.GetData();
        }
        
        inline UseItemOnC2SPacket DeserializeUseItemOnC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            UseItemOnC2SPacket packet;
            packet.hand = reader.ReadVarInt();              // Hand (VarInt)
            packet.blockX = reader.ReadInt();               // Block X
            packet.blockY = reader.ReadInt();               // Block Y
            packet.blockZ = reader.ReadInt();               // Block Z
            packet.direction = reader.ReadVarInt();         // Direction/Face (VarInt)
            packet.cursorX = reader.ReadFloat();            // Cursor X
            packet.cursorY = reader.ReadFloat();            // Cursor Y
            packet.cursorZ = reader.ReadFloat();            // Cursor Z
            packet.insideBlock = reader.ReadByte() != 0;    // Inside block flag
            packet.sequence = reader.ReadVarInt();          // Sequence number (VarInt)
            return packet;
        }

        // ---- ChunkDataS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const ChunkDataS2CPacket& packet) {
            Network::PacketBuffer buffer(4096); // Reserve more space for chunk data
            
            // Calculate total data size first
            size_t dataSize = 0;
            for (const auto& section : packet.sections) {
                dataSize += sizeof(uint16_t); // blockCount
                dataSize += sizeof(uint8_t);  // bitsPerEntry
                if (!section.palette.empty()) {
                    dataSize += VarInt::GetSize(static_cast<uint32_t>(section.palette.size()));
                    for (uint32_t id : section.palette) {
                        dataSize += VarInt::GetSize(id);
                    }
                }
                dataSize += VarInt::GetSize(static_cast<uint32_t>(section.dataArray.size()));
                dataSize += section.dataArray.size() * sizeof(uint64_t);
            }
            if (packet.groundUpContinuous) {
                dataSize += packet.biomeData.size();
            }
            
            // Write packet structure following Minecraft protocol
            // Note: Packet ID and length prefix are handled by the network layer
            
            // 1. Chunk coordinates
            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            
            // 2. Ground-up continuous flag
            buffer.WriteByte(packet.groundUpContinuous ? 1 : 0);
            
            // 3. Primary bitmask (VarInt)
            buffer.WriteVarInt(packet.primaryBitmask);
            
            // 4. Data size (VarInt)
            buffer.WriteVarInt(static_cast<uint32_t>(dataSize));
            
            // 5. Data array - Write sections in ascending Y order
            for (const auto& section : packet.sections) {
                // Block count (short)
                buffer.WriteShort(section.blockCount);
                
                // Bits per entry
                buffer.WriteByte(section.bitsPerEntry);
                
                // Palette (if present)
                if (section.bitsPerEntry > 0 && section.bitsPerEntry <= 8) {
                    // Palette mode
                    buffer.WriteVarInt(static_cast<uint32_t>(section.palette.size()));
                    for (uint32_t blockStateId : section.palette) {
                        buffer.WriteVarInt(blockStateId);
                    }
                }
                
                // Data array length
                buffer.WriteVarInt(static_cast<uint32_t>(section.dataArray.size()));
                
                // Data array (packed longs)
                for (uint64_t data : section.dataArray) {
                    buffer.WriteLong(data);
                }
            }
            
            // 6. Biome data (if ground-up continuous)
            if (packet.groundUpContinuous && !packet.biomeData.empty()) {
                buffer.WriteBytes(packet.biomeData);
            }
            
            return buffer.GetData();
        }
        
        inline ChunkDataS2CPacket DeserializeChunkDataS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkDataS2CPacket packet;
            
            // Read chunk coordinates
            packet.chunkX = reader.ReadInt();
            packet.chunkZ = reader.ReadInt();
            
            // Read ground-up continuous flag
            packet.groundUpContinuous = reader.ReadByte() != 0;
            
            // Read primary bitmask
            packet.primaryBitmask = reader.ReadVarInt();
            
            // Read data size
            uint32_t dataSize = reader.ReadVarInt();
            
            // Count sections from bitmask
            uint32_t sectionCount = 0;
            for (int i = 0; i < 24; ++i) { // Max 24 sections for world height 384
                if (packet.primaryBitmask & (1 << i)) {
                    sectionCount++;
                }
            }
            
            // Read section data
            packet.sections.reserve(sectionCount);
            for (uint32_t i = 0; i < sectionCount; ++i) {
                ChunkDataS2CPacket::SectionData section;
                
                // Read block count
                section.blockCount = reader.ReadShort();
                
                // Read bits per entry
                section.bitsPerEntry = reader.ReadByte();
                
                // Read palette if present
                if (section.bitsPerEntry > 0 && section.bitsPerEntry <= 8) {
                    uint32_t paletteSize = reader.ReadVarInt();
                    section.palette.reserve(paletteSize);
                    for (uint32_t j = 0; j < paletteSize; ++j) {
                        section.palette.push_back(reader.ReadVarInt());
                    }
                }
                
                // Read data array
                uint32_t dataArraySize = reader.ReadVarInt();
                section.dataArray.reserve(dataArraySize);
                for (uint32_t j = 0; j < dataArraySize; ++j) {
                    section.dataArray.push_back(reader.ReadLong());
                }
                
                packet.sections.push_back(std::move(section));
            }
            
            // Read biome data if ground-up continuous
            if (packet.groundUpContinuous && reader.HasMore()) {
                size_t biomeSize = std::min(reader.Remaining(), size_t(256));
                packet.biomeData = reader.ReadBytes(biomeSize);
            }
            
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

        // ---- UnloadChunkS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const UnloadChunkS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.chunkX);
            buffer.WriteInt(packet.chunkZ);
            return buffer.GetData();
        }
        
        inline UnloadChunkS2CPacket DeserializeUnloadChunkS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            UnloadChunkS2CPacket packet;
            packet.chunkX = reader.ReadInt();
            packet.chunkZ = reader.ReadInt();
            packet.timestamp = std::chrono::steady_clock::now();
            return packet;
        }

        // ---- Chunk batch packets ----

        inline std::vector<uint8_t> Serialize(const ChunkBatchStartS2CPacket&) {
            return {}; // Empty marker packet
        }

        inline std::vector<uint8_t> Serialize(const ChunkBatchFinishedS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteInt(packet.batchSize);
            return buffer.GetData();
        }

        inline ChunkBatchFinishedS2CPacket DeserializeChunkBatchFinishedS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkBatchFinishedS2CPacket packet;
            packet.batchSize = reader.ReadInt();
            return packet;
        }

        // ---- SetChunkCacheRadiusS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const SetChunkCacheRadiusS2CPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteVarInt(packet.viewDistance);
            return buffer.GetData();
        }

        inline SetChunkCacheRadiusS2CPacket DeserializeSetChunkCacheRadiusS2C(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            SetChunkCacheRadiusS2CPacket packet;
            packet.viewDistance = reader.ReadVarInt();
            return packet;
        }

        inline std::vector<uint8_t> Serialize(const ChunkBatchAckC2SPacket& packet) {
            Network::PacketBuffer buffer;
            buffer.WriteFloat(packet.desiredChunksPerTick);
            return buffer.GetData();
        }

        inline ChunkBatchAckC2SPacket DeserializeChunkBatchAckC2S(const std::vector<uint8_t>& data) {
            Network::PacketReader reader(data);
            ChunkBatchAckC2SPacket packet;
            packet.desiredChunksPerTick = reader.ReadFloat();
            return packet;
        }

        // ---- HeldItemChangeC2SPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const HeldItemChangeC2SPacket& packet) {
            PacketBuffer buffer;
            buffer.WriteShort(packet.slot);
            return buffer.GetData();
        }
        inline HeldItemChangeC2SPacket DeserializeHeldItemChangeC2S(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            HeldItemChangeC2SPacket packet;
            packet.slot = reader.ReadShort();
            return packet;
        }

        // ---- HotbarSyncS2CPacket Serialization ----
        inline std::vector<uint8_t> Serialize(const HotbarSyncS2CPacket& packet) {
            PacketBuffer buffer;
            for (int i = 0; i < 9; i++) buffer.WriteShort(packet.slots[i]);
            return buffer.GetData();
        }
        inline HotbarSyncS2CPacket DeserializeHotbarSyncS2C(const std::vector<uint8_t>& data) {
            PacketReader reader(data);
            HotbarSyncS2CPacket packet;
            for (int i = 0; i < 9; i++) packet.slots[i] = static_cast<uint16_t>(reader.ReadShort());
            return packet;
        }

        // ---- Helper function to serialize any packet based on type ----
        template<typename PacketType>
        inline std::vector<uint8_t> SerializePacket(const PacketType& packet) {
            return Serialize(packet);
        }

    } // namespace Serialization

} // namespace Network