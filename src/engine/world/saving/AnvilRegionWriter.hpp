#pragma once

#include "../Chunk.hpp"
#include "../../game/WorldMath.hpp"
#include "../../core/Log.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <cstdint>
#include <array>

namespace Game {

    // Location entry in region file header
    struct AnvilChunkLocation {
        uint32_t sectorOffset = 0;  // In 4KiB sectors from file start
        uint8_t sectorCount = 0;    // Number of 4KiB sectors

        AnvilChunkLocation() = default;
        AnvilChunkLocation(uint32_t offset, uint8_t count) 
            : sectorOffset(offset), sectorCount(count) {}

        bool isEmpty() const { return sectorOffset == 0 && sectorCount == 0; }
        
        // Pack into 4-byte big-endian format
        uint32_t pack() const {
            return (sectorOffset << 8) | sectorCount;
        }
        
        // Unpack from 4-byte big-endian format
        static AnvilChunkLocation unpack(uint32_t packed) {
            return AnvilChunkLocation((packed >> 8) & 0xFFFFFF, packed & 0xFF);
        }
    };

    // Anvil region file writer for true .mca format
    class AnvilRegionWriter {
    public:
        static constexpr int REGION_SIZE = 32;           // 32x32 chunks per region
        static constexpr int SECTOR_SIZE = 4096;         // 4KiB sectors
        static constexpr int HEADER_SIZE = 8192;         // 8KiB header
        static constexpr int CHUNKS_PER_REGION = 1024;  // 32*32

        explicit AnvilRegionWriter(const std::string& filePath);
        ~AnvilRegionWriter();

        // Initialize the region file (create if needed)
        bool Initialize();

        // Write a chunk to the region file
        bool WriteChunk(int localX, int localZ, const Chunk& chunk);

        // Flush all pending writes and close file
        bool Finalize();

        // Check if the region writer is ready
        bool IsReady() const { return m_initialized && m_fileStream.is_open(); }

        // Get the file path
        const std::string& GetFilePath() const { return m_filePath; }

        // Get region coordinates from chunk coordinates
        static void ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ, 
                                  int& localX, int& localZ);

        // Generate region file path from coordinates
        static std::string GenerateRegionFilePath(const std::string& worldPath, 
                                                  int regionX, int regionZ);

    private:
        std::string m_filePath;
        std::fstream m_fileStream;
        bool m_initialized = false;

        // Region file header data
        std::array<AnvilChunkLocation, CHUNKS_PER_REGION> m_locations;
        std::array<uint32_t, CHUNKS_PER_REGION> m_timestamps;

        // Sector allocation tracking
        std::vector<bool> m_freeSectors;

        // === CORE IMPLEMENTATION ===

        // Load existing header or create new one
        bool LoadOrCreateHeader();

        // Save header to file
        bool SaveHeader();

        // Allocate sectors for chunk data
        int AllocateSectors(int sectorsNeeded);

        // Mark sectors as free
        void MarkSectorsFree(int startSector, int count);

        // Build sector allocation map
        void BuildSectorMap();

        // === CHUNK SERIALIZATION ===

        // Convert chunk to Anvil NBT format
        std::vector<uint8_t> SerializeChunkToNBT(const Chunk& chunk);

        // Build NBT structure for chunk
        void BuildChunkNBT(const Chunk& chunk, std::vector<uint8_t>& nbtData);

        // Serialize heightmaps to NBT format
        void SerializeHeightmaps(const Chunk& chunk, std::vector<uint8_t>& nbtData);

        // Serialize sections to NBT format
        void SerializeSections(const Chunk& chunk, std::vector<uint8_t>& nbtData);

        // === NBT WRITING HELPERS ===

        // Write NBT tag header
        void WriteTagHeader(std::vector<uint8_t>& data, uint8_t tagType, const std::string& name = "");

        // Write various NBT data types
        void WriteInt(std::vector<uint8_t>& data, int32_t value);
        void WriteLong(std::vector<uint8_t>& data, int64_t value);
        void WriteString(std::vector<uint8_t>& data, const std::string& value);
        void WriteLongArray(std::vector<uint8_t>& data, const std::vector<int64_t>& values);
        void WriteByteArray(std::vector<uint8_t>& data, const std::vector<int8_t>& values);

        // Write compound and list tags
        void WriteCompoundStart(std::vector<uint8_t>& data, const std::string& name = "");
        void WriteCompoundEnd(std::vector<uint8_t>& data);
        void WriteListStart(std::vector<uint8_t>& data, uint8_t elementType, int32_t count, const std::string& name = "");

        // === COMPRESSION ===

        // Compress NBT data with Zlib
        std::vector<uint8_t> CompressZlib(const std::vector<uint8_t>& data);

        // === UTILITY FUNCTIONS ===

        // Convert local coordinates to region index
        int CoordsToIndex(int localX, int localZ) const;

        // Get current Unix timestamp
        uint32_t GetCurrentTimestamp() const;

        // Validate local coordinates
        bool IsValidLocalCoords(int localX, int localZ) const;

        // Calculate heightmaps for chunk
        std::vector<int64_t> CalculateMotionBlockingHeightmap(const Chunk& chunk);
        std::vector<int64_t> CalculateWorldSurfaceHeightmap(const Chunk& chunk);

        // Pack heightmap data into long array format
        std::vector<int64_t> PackHeightmapToLongs(const std::vector<int>& heights);
    };

    // === UTILITY FUNCTIONS ===

    // Create all necessary region files for a chunk
    bool EnsureRegionFileExists(const std::string& worldPath, Math::ChunkPos chunkPos);

    // Save a chunk to the appropriate region file
    bool SaveChunkToAnvilFormat(const std::string& worldPath, const Chunk& chunk);

    // Create world directory structure
    bool CreateMinecraftWorldStructure(const std::string& worldPath);

} // namespace Game