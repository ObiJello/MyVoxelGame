// File: src/tools/RegionDumper.hpp
#pragma once

#include "RegionFile.hpp"
#include "NBTParser.hpp"
#include <string>
#include <vector>
#include <memory>

namespace World {

    // Chunk data read from region file
    struct ChunkData {
        int localX, localZ;      // Local coordinates within region (0-31)
        int worldX, worldZ;      // World chunk coordinates
        uint32_t length;         // Compressed data length
        uint8_t version;         // Compression version (1=GZip, 2=ZLib, 3=Uncompressed)
        std::vector<uint8_t> compressedData;  // Raw compressed data
        std::vector<uint8_t> uncompressedData; // Decompressed NBT data
        NBTTagPtr rootTag;       // Parsed NBT root compound
        bool isValid;            // Whether chunk data was successfully parsed

        ChunkData() : localX(0), localZ(0), worldX(0), worldZ(0),
                     length(0), version(0), isValid(false) {}
    };

    // Utility class for dumping region file contents
    class RegionDumper {
    public:
        // Dump region file by coordinates (looks for standard path structure)
        static bool DumpRegion(int regionX, int regionZ, const std::string& worldPath = ".");

        // Dump region file by direct path
        static bool DumpRegionFile(const std::string& filePath);

        // Dump specific chunk from region
        static bool DumpChunk(int regionX, int regionZ, int localX, int localZ,
                             const std::string& worldPath = ".");

        // Read chunk data from region file
        static ChunkData ReadChunkData(RegionFile& regionFile, int localX, int localZ);

        // Enhanced read chunk data with debug support
        static ChunkData ReadChunkDataWithDebug(RegionFile& regionFile, int localX, int localZ);

        // Print chunk header information only (no NBT parsing)
        static void PrintChunkHeaders(RegionFile& regionFile);

        // Print detailed chunk NBT data
        static void PrintChunkNBT(const ChunkData& chunkData, bool verbose = false);

        // Decompress chunk data (handles ZLib, GZip, and uncompressed)
        static bool DecompressChunkData(ChunkData& chunkData);

        // Enhanced decompress with debug logging
        static bool DecompressChunkDataWithDebug(ChunkData& chunkData);

        // Validate NBT structure for chunk data (supports both pre-1.18 and 1.18+ formats)
        static bool ValidateChunkNBT(const NBTTagPtr& rootTag);

        // Enhanced validation with 1.18+ support
        static bool ValidateChunkNBT118Plus(const NBTTagPtr& rootTag);

        // Extract useful information from chunk NBT for verification
        static void ExtractChunkInfo(const NBTTagPtr& rootTag);

        // Enhanced chunk info extraction with 1.18+ support
        static void ExtractChunkInfo118Plus(const NBTTagPtr& rootTag);

    private:
        RegionDumper() = delete; // Static utility class

        // Helper to get compression type name
        static std::string GetCompressionTypeName(uint8_t version);

        // Helper to format timestamp
        static std::string FormatTimestamp(uint32_t timestamp);

        // Helper to convert region+local coordinates to world coordinates
        static void LocalToWorld(int regionX, int regionZ, int localX, int localZ,
                                int& worldX, int& worldZ);
    };

} // namespace World