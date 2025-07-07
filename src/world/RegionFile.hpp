// File: src/world/RegionFile.hpp
#pragma once

#include <string>
#include <array>
#include <memory>
#include <fstream>
#include <cstdint>

namespace World {

    // Represents a chunk's location in the region file
    struct ChunkLoc {
        uint32_t sectorOffset;  // 24-bit offset in 4096-byte sectors
        uint8_t sectorCount;    // Number of sectors this chunk occupies

        ChunkLoc() : sectorOffset(0), sectorCount(0) {}
        ChunkLoc(uint32_t offset, uint8_t count) : sectorOffset(offset), sectorCount(count) {}

        bool isEmpty() const { return sectorOffset == 0 && sectorCount == 0; }
    };

    // Represents a Minecraft region file (.mca)
    class RegionFile {
    public:
        static constexpr int REGION_SIZE = 32;          // 32x32 chunks per region
        static constexpr int SECTOR_SIZE = 4096;        // 4KB per sector
        static constexpr int HEADER_SIZE = 8192;        // 8KB header (location + timestamp tables)
        static constexpr int CHUNKS_PER_REGION = REGION_SIZE * REGION_SIZE; // 1024

        explicit RegionFile(const std::string& filePath);
        ~RegionFile() = default;

        // Load the region file header (location and timestamp tables)
        bool LoadHeader();

        // Get chunk location info for local coordinates (0-31)
        ChunkLoc GetLocation(int localX, int localZ) const;

        // Get chunk timestamp for local coordinates (0-31)
        uint32_t GetTimestamp(int localX, int localZ) const;

        // Get file path
        const std::string& GetFilePath() const { return filePath; }

        // Check if file exists and header is loaded
        bool IsValid() const { return headerLoaded && fileExists; }

        // Get file stream for reading chunk data
        std::ifstream& GetFileStream() { return fileStream; }

    private:
        std::string filePath;
        bool headerLoaded;
        bool fileExists;

        // Region file data
        std::array<ChunkLoc, CHUNKS_PER_REGION> locationTable;
        std::array<uint32_t, CHUNKS_PER_REGION> timestampTable;

        // File stream for reading chunk data
        mutable std::ifstream fileStream;

        // Convert local coordinates to table index
        static int CoordinatesToIndex(int localX, int localZ);

        // Parse 32-bit big-endian value from location table
        static ChunkLoc ParseLocationEntry(uint32_t entry);
    };

} // namespace World