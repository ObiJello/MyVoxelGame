// File: src/world/RegionDumper.cpp
#include "RegionDumper.hpp"
#include "RegionFileCache.hpp"
#include "../core/Log.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <ctime>
#include <zlib.h>

namespace World {

    bool RegionDumper::DumpRegion(int regionX, int regionZ, const std::string& worldPath) {
        Log::Info("=== REGION DUMP START ===");
        Log::Info("Region coordinates: (%d, %d)", regionX, regionZ);
        Log::Info("World path: %s", worldPath.c_str());

        // Get region file from cache
        auto regionFile = RegionFileCache::Instance().GetRegionFile(regionX, regionZ, worldPath);
        if (!regionFile || !regionFile->IsValid()) {
            Log::Error("Failed to load region file for region (%d, %d)", regionX, regionZ);
            return false;
        }

        std::cout << "================================================" << std::endl;
        std::cout << "REGION FILE: " << regionFile->GetFilePath() << std::endl;
        std::cout << "REGION COORDINATES: (" << regionX << ", " << regionZ << ")" << std::endl;
        std::cout << "================================================" << std::endl;

        // Print chunk headers
        PrintChunkHeaders(*regionFile);

        // Read and dump each chunk
        int processedChunks = 0;
        int validChunks = 0;

        for (int localZ = 0; localZ < RegionFile::REGION_SIZE; ++localZ) {
            for (int localX = 0; localX < RegionFile::REGION_SIZE; ++localX) {
                ChunkLoc location = regionFile->GetLocation(localX, localZ);

                if (location.isEmpty()) {
                    continue; // Skip empty chunks
                }

                processedChunks++;

                // Read chunk data
                ChunkData chunkData = ReadChunkData(*regionFile, localX, localZ);

                if (chunkData.isValid) {
                    validChunks++;

                    std::cout << "\n================================================" << std::endl;
                    std::cout << "CHUNK (" << localX << ", " << localZ << ") - World ("
                             << chunkData.worldX << ", " << chunkData.worldZ << ")" << std::endl;
                    std::cout << "================================================" << std::endl;

                    PrintChunkNBT(chunkData, false); // Set to true for verbose output

                    // Extract and display key chunk information
                    if (chunkData.rootTag) {
                        ExtractChunkInfo(chunkData.rootTag);
                    }
                } else {
                    Log::Warning("Failed to read chunk data for (%d, %d)", localX, localZ);
                }
            }
        }

        std::cout << "\n================================================" << std::endl;
        std::cout << "REGION DUMP SUMMARY" << std::endl;
        std::cout << "================================================" << std::endl;
        std::cout << "Total chunks processed: " << processedChunks << std::endl;
        std::cout << "Valid chunks: " << validChunks << std::endl;
        std::cout << "Failed chunks: " << (processedChunks - validChunks) << std::endl;

        Log::Info("=== REGION DUMP COMPLETE ===");
        return validChunks > 0;
    }

    bool RegionDumper::DumpRegionFile(const std::string& filePath) {
        Log::Info("Dumping region file: %s", filePath.c_str());

        auto regionFile = RegionFileCache::Instance().GetRegionFile(filePath);
        if (!regionFile || !regionFile->IsValid()) {
            Log::Error("Failed to load region file: %s", filePath.c_str());
            return false;
        }

        std::cout << "================================================" << std::endl;
        std::cout << "REGION FILE: " << filePath << std::endl;
        std::cout << "================================================" << std::endl;

        PrintChunkHeaders(*regionFile);

        return true;
    }

    bool RegionDumper::DumpChunk(int regionX, int regionZ, int localX, int localZ,
                                const std::string& worldPath) {
        if (localX < 0 || localX >= RegionFile::REGION_SIZE ||
            localZ < 0 || localZ >= RegionFile::REGION_SIZE) {
            Log::Error("Invalid local coordinates: (%d, %d)", localX, localZ);
            return false;
        }

        Log::Info("Dumping single chunk: region (%d, %d), local (%d, %d)",
                 regionX, regionZ, localX, localZ);

        auto regionFile = RegionFileCache::Instance().GetRegionFile(regionX, regionZ, worldPath);
        if (!regionFile || !regionFile->IsValid()) {
            Log::Error("Failed to load region file for region (%d, %d)", regionX, regionZ);
            return false;
        }

        ChunkData chunkData = ReadChunkData(*regionFile, localX, localZ);

        if (!chunkData.isValid) {
            Log::Error("Failed to read chunk data for (%d, %d)", localX, localZ);
            return false;
        }

        std::cout << "================================================" << std::endl;
        std::cout << "CHUNK (" << localX << ", " << localZ << ") - World ("
                 << chunkData.worldX << ", " << chunkData.worldZ << ")" << std::endl;
        std::cout << "================================================" << std::endl;

        PrintChunkNBT(chunkData, true); // Verbose output for single chunk

        if (chunkData.rootTag) {
            ExtractChunkInfo(chunkData.rootTag);
        }

        return true;
    }

    ChunkData RegionDumper::ReadChunkData(RegionFile& regionFile, int localX, int localZ) {
        ChunkData chunkData;
        chunkData.localX = localX;
        chunkData.localZ = localZ;

        // Convert to world coordinates (approximate - we'll get exact coords from NBT)
        LocalToWorld(0, 0, localX, localZ, chunkData.worldX, chunkData.worldZ);

        // Get chunk location
        ChunkLoc location = regionFile.GetLocation(localX, localZ);
        if (location.isEmpty()) {
            Log::Debug("Chunk (%d, %d) is empty", localX, localZ);
            return chunkData;
        }

        // Open file stream
        auto& fileStream = regionFile.GetFileStream();
        if (!fileStream.is_open()) {
            Log::Error("Region file stream is not open");
            return chunkData;
        }

        // Seek to chunk data
        size_t chunkOffset = static_cast<size_t>(location.sectorOffset) * RegionFile::SECTOR_SIZE;
        fileStream.seekg(chunkOffset);

        if (fileStream.fail()) {
            Log::Error("Failed to seek to chunk offset %zu", chunkOffset);
            return chunkData;
        }

        // Read chunk header (4 bytes length + 1 byte version)
        uint8_t header[5];
        fileStream.read(reinterpret_cast<char*>(header), 5);

        if (fileStream.gcount() != 5) {
            Log::Error("Failed to read chunk header for (%d, %d)", localX, localZ);
            return chunkData;
        }

        // Parse header (big-endian)
        chunkData.length = (static_cast<uint32_t>(header[0]) << 24) |
                          (static_cast<uint32_t>(header[1]) << 16) |
                          (static_cast<uint32_t>(header[2]) << 8) |
                          static_cast<uint32_t>(header[3]);
        chunkData.version = header[4];

        // Validate length
        if (chunkData.length == 0 || chunkData.length > 1024 * 1024) { // 1MB sanity check
            Log::Error("Invalid chunk length: %u", chunkData.length);
            return chunkData;
        }

        // Read compressed data (length includes version byte, so subtract 1)
        uint32_t dataLength = chunkData.length - 1;
        chunkData.compressedData.resize(dataLength);

        fileStream.read(reinterpret_cast<char*>(chunkData.compressedData.data()), dataLength);

        if (static_cast<uint32_t>(fileStream.gcount()) != dataLength) {
            Log::Error("Failed to read chunk data for (%d, %d): expected %u bytes, got %lld",
                      localX, localZ, dataLength, static_cast<long long>(fileStream.gcount()));
            return chunkData;
        }

        // Decompress data
        if (!DecompressChunkData(chunkData)) {
            Log::Error("Failed to decompress chunk data for (%d, %d)", localX, localZ);
            return chunkData;
        }

        // Parse NBT data
        chunkData.rootTag = NBTParser::Parse(chunkData.uncompressedData);
        if (!chunkData.rootTag) {
            Log::Error("Failed to parse NBT data for chunk (%d, %d)", localX, localZ);
            return chunkData;
        }

        // Extract actual world coordinates from NBT
        auto compound = std::dynamic_pointer_cast<NBTTagCompound>(chunkData.rootTag);
        if (compound) {
            chunkData.worldX = compound->GetValue<int32_t>("xPos", chunkData.worldX);
            chunkData.worldZ = compound->GetValue<int32_t>("zPos", chunkData.worldZ);
        }

        chunkData.isValid = true;
        return chunkData;
    }

    void RegionDumper::PrintChunkHeaders(RegionFile& regionFile) {
        std::cout << "\nCHUNK LOCATION TABLE:" << std::endl;
        std::cout << "=====================" << std::endl;

        int nonEmptyChunks = 0;

        for (int localZ = 0; localZ < RegionFile::REGION_SIZE; ++localZ) {
            for (int localX = 0; localX < RegionFile::REGION_SIZE; ++localX) {
                ChunkLoc location = regionFile.GetLocation(localX, localZ);
                uint32_t timestamp = regionFile.GetTimestamp(localX, localZ);

                if (!location.isEmpty()) {
                    nonEmptyChunks++;

                    std::cout << "Chunk (" << std::setw(2) << localX << ", " << std::setw(2) << localZ << "): "
                             << "offset=" << std::setw(6) << location.sectorOffset
                             << ", sectors=" << std::setw(2) << static_cast<int>(location.sectorCount)
                             << ", timestamp=" << FormatTimestamp(timestamp) << std::endl;
                }
            }
        }

        std::cout << "\nSUMMARY: " << nonEmptyChunks << " non-empty chunks out of "
                 << (RegionFile::REGION_SIZE * RegionFile::REGION_SIZE) << " total slots" << std::endl;
    }

    void RegionDumper::PrintChunkNBT(const ChunkData& chunkData, bool verbose) {
        if (!chunkData.isValid || !chunkData.rootTag) {
            std::cout << "Invalid chunk data" << std::endl;
            return;
        }

        std::cout << "Chunk Info:" << std::endl;
        std::cout << "  Local coords: (" << chunkData.localX << ", " << chunkData.localZ << ")" << std::endl;
        std::cout << "  World coords: (" << chunkData.worldX << ", " << chunkData.worldZ << ")" << std::endl;
        std::cout << "  Data length: " << chunkData.length << " bytes" << std::endl;
        std::cout << "  Compression: " << GetCompressionTypeName(chunkData.version) << std::endl;
        std::cout << "  Compressed size: " << chunkData.compressedData.size() << " bytes" << std::endl;
        std::cout << "  Uncompressed size: " << chunkData.uncompressedData.size() << " bytes" << std::endl;

        if (verbose) {
            std::cout << "\nFull NBT Structure:" << std::endl;
            std::cout << "===================" << std::endl;
            chunkData.rootTag->Print(std::cout, 0);
        } else {
            std::cout << "\nNBT Root Tag:" << std::endl;
            std::cout << "=============" << std::endl;
            std::cout << chunkData.rootTag->ToString() << std::endl;
        }

        // Validate NBT structure
        bool isValid = ValidateChunkNBT(chunkData.rootTag);
        std::cout << "\nNBT Validation: " << (isValid ? "PASSED" : "FAILED") << std::endl;
    }

    bool RegionDumper::DecompressChunkData(ChunkData& chunkData) {
        if (chunkData.compressedData.empty()) {
            Log::Error("No compressed data to decompress");
            return false;
        }

        // Determine decompression method based on version
        bool success = false;

        if (chunkData.version == 1) {
            // GZip compression (rarely used)
            Log::Debug("Using GZip decompression");
            // For simplicity, we'll treat it the same as ZLib for now
            // In a full implementation, you might need different handling
        } else if (chunkData.version == 2) {
            // ZLib compression (standard)
            Log::Debug("Using ZLib decompression");
        } else {
            Log::Error("Unknown compression version: %u", chunkData.version);
            return false;
        }

        // Decompress using zlib
        uLongf destLen = 1024 * 1024; // 1MB initial buffer
        chunkData.uncompressedData.resize(destLen);

        int result = uncompress(
            chunkData.uncompressedData.data(),
            &destLen,
            chunkData.compressedData.data(),
            chunkData.compressedData.size()
        );

        if (result == Z_OK) {
            chunkData.uncompressedData.resize(destLen);
            Log::Debug("Decompression successful: %u -> %lu bytes",
                      static_cast<uint32_t>(chunkData.compressedData.size()), destLen);
            success = true;
        } else {
            const char* errorMsg = "Unknown error";
            switch (result) {
                case Z_MEM_ERROR: errorMsg = "Not enough memory"; break;
                case Z_BUF_ERROR: errorMsg = "Output buffer too small"; break;
                case Z_DATA_ERROR: errorMsg = "Input data corrupted"; break;
            }
            Log::Error("ZLib decompression failed: %s (code: %d)", errorMsg, result);

            // Try with a larger buffer
            if (result == Z_BUF_ERROR) {
                destLen = 4 * 1024 * 1024; // 4MB buffer
                chunkData.uncompressedData.resize(destLen);

                result = uncompress(
                    chunkData.uncompressedData.data(),
                    &destLen,
                    chunkData.compressedData.data(),
                    chunkData.compressedData.size()
                );

                if (result == Z_OK) {
                    chunkData.uncompressedData.resize(destLen);
                    Log::Info("Decompression successful with larger buffer: %lu bytes", destLen);
                    success = true;
                }
            }
        }

        return success;
    }

    bool RegionDumper::ValidateChunkNBT(const NBTTagPtr& rootTag) {
        if (!rootTag) {
            Log::Warning("Root tag is null");
            return false;
        }

        // Root should be a compound tag
        auto rootCompound = std::dynamic_pointer_cast<NBTTagCompound>(rootTag);
        if (!rootCompound) {
            Log::Warning("Root tag is not a compound");
            return false;
        }

        // Check for essential chunk data
        bool hasLevel = rootCompound->HasTag("Level");
        // Note: DataVersion is optional for older chunks
        // bool hasDataVersion = rootCompound->HasTag("DataVersion");

        if (!hasLevel) {
            Log::Warning("Chunk missing 'Level' tag");
            return false;
        }

        // Get Level compound
        auto levelTag = std::dynamic_pointer_cast<NBTTagCompound>(rootCompound->GetTag("Level"));
        if (!levelTag) {
            Log::Warning("'Level' tag is not a compound");
            return false;
        }

        // Check for essential level data
        bool hasXPos = levelTag->HasTag("xPos");
        bool hasZPos = levelTag->HasTag("zPos");
        // Note: Sections and Biomes are optional for some chunk formats
        // bool hasSections = levelTag->HasTag("Sections");
        // bool hasBiomes = levelTag->HasTag("Biomes");

        if (!hasXPos || !hasZPos) {
            Log::Warning("Chunk missing position tags (xPos/zPos)");
            return false;
        }

        Log::Debug("NBT validation passed - chunk has required structure");
        return true;
    }

    void RegionDumper::ExtractChunkInfo(const NBTTagPtr& rootTag) {
        auto rootCompound = std::dynamic_pointer_cast<NBTTagCompound>(rootTag);
        if (!rootCompound) {
            return;
        }

        std::cout << "\nExtracted Chunk Information:" << std::endl;
        std::cout << "============================" << std::endl;

        // Data version
        int32_t dataVersion = rootCompound->GetValue<int32_t>("DataVersion", -1);
        if (dataVersion != -1) {
            std::cout << "Data Version: " << dataVersion << std::endl;
        }

        // Get Level data
        auto levelTag = std::dynamic_pointer_cast<NBTTagCompound>(rootCompound->GetTag("Level"));
        if (!levelTag) {
            std::cout << "No Level data found" << std::endl;
            return;
        }

        // Position
        int32_t xPos = levelTag->GetValue<int32_t>("xPos", 0);
        int32_t zPos = levelTag->GetValue<int32_t>("zPos", 0);
        std::cout << "Chunk Position: (" << xPos << ", " << zPos << ")" << std::endl;

        // Status
        std::string status = levelTag->GetValue<std::string>("Status", "unknown");
        std::cout << "Generation Status: " << status << std::endl;

        // Last update
        int64_t lastUpdate = levelTag->GetValue<int64_t>("LastUpdate", 0);
        std::cout << "Last Update: " << lastUpdate << std::endl;

        // Inhabitance time
        int64_t inhabitedTime = levelTag->GetValue<int64_t>("InhabitedTime", 0);
        std::cout << "Inhabited Time: " << inhabitedTime << " ticks" << std::endl;

        // Y position (for 1.18+ chunks)
        int32_t yPos = levelTag->GetValue<int32_t>("yPos", INT32_MIN);
        if (yPos != INT32_MIN) {
            std::cout << "Y Position: " << yPos << std::endl;
        }

        // Sections
        auto sectionsTag = std::dynamic_pointer_cast<NBTTagList>(levelTag->GetTag("Sections"));
        if (sectionsTag) {
            std::cout << "Sections: " << sectionsTag->value.size() << " section(s)" << std::endl;

            // Show section Y levels
            for (size_t i = 0; i < sectionsTag->value.size() && i < 5; ++i) { // Limit to first 5
                auto section = std::dynamic_pointer_cast<NBTTagCompound>(sectionsTag->value[i]);
                if (section) {
                    int8_t sectionY = section->GetValue<int8_t>("Y", 0);
                    std::cout << "  Section " << i << ": Y=" << static_cast<int>(sectionY) << std::endl;
                }
            }
            if (sectionsTag->value.size() > 5) {
                std::cout << "  ... and " << (sectionsTag->value.size() - 5) << " more sections" << std::endl;
            }
        }

        // Biomes
        auto biomesTag = levelTag->GetTag("Biomes");
        if (biomesTag) {
            if (auto biomeArray = std::dynamic_pointer_cast<NBTTagIntArray>(biomesTag)) {
                std::cout << "Biomes: " << biomeArray->value.size() << " biome entries" << std::endl;
            } else if (auto biomeByteArray = std::dynamic_pointer_cast<NBTTagByteArray>(biomesTag)) {
                std::cout << "Biomes: " << biomeByteArray->value.size() << " biome bytes" << std::endl;
            }
        }

        // Entities
        auto entitiesTag = std::dynamic_pointer_cast<NBTTagList>(levelTag->GetTag("Entities"));
        if (entitiesTag) {
            std::cout << "Entities: " << entitiesTag->value.size() << " entity(ies)" << std::endl;
        }

        // Block entities
        auto blockEntitiesTag = std::dynamic_pointer_cast<NBTTagList>(levelTag->GetTag("TileEntities"));
        if (!blockEntitiesTag) {
            blockEntitiesTag = std::dynamic_pointer_cast<NBTTagList>(levelTag->GetTag("BlockEntities"));
        }
        if (blockEntitiesTag) {
            std::cout << "Block Entities: " << blockEntitiesTag->value.size() << " block entity(ies)" << std::endl;
        }

        // Heightmaps
        auto heightmapsTag = std::dynamic_pointer_cast<NBTTagCompound>(levelTag->GetTag("Heightmaps"));
        if (heightmapsTag) {
            std::cout << "Heightmaps: " << heightmapsTag->value.size() << " heightmap(s)" << std::endl;
            for (const auto& [name, heightmap] : heightmapsTag->value) {
                std::cout << "  - " << name << std::endl;
            }
        }
    }

    std::string RegionDumper::GetCompressionTypeName(uint8_t version) {
        switch (version) {
            case 1: return "GZip";
            case 2: return "ZLib";
            case 3: return "Uncompressed";
            default: return "Unknown (" + std::to_string(version) + ")";
        }
    }

    std::string RegionDumper::FormatTimestamp(uint32_t timestamp) {
        if (timestamp == 0) {
            return "Never";
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm = std::localtime(&time);

        std::ostringstream oss;
        oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    void RegionDumper::LocalToWorld(int regionX, int regionZ, int localX, int localZ,
                                   int& worldX, int& worldZ) {
        worldX = regionX * RegionFile::REGION_SIZE + localX;
        worldZ = regionZ * RegionFile::REGION_SIZE + localZ;
    }

    ChunkData RegionDumper::ReadChunkDataWithDebug(RegionFile& regionFile, int localX, int localZ) {
        ChunkData chunkData;
        chunkData.localX = localX;
        chunkData.localZ = localZ;

        // Convert to world coordinates (approximate - we'll get exact coords from NBT)
        LocalToWorld(0, 0, localX, localZ, chunkData.worldX, chunkData.worldZ);

        Log::Debug("=== Reading chunk data for (%d, %d) ===", localX, localZ);

        // Get chunk location
        ChunkLoc location = regionFile.GetLocation(localX, localZ);
        if (location.isEmpty()) {
            Log::Debug("Chunk (%d, %d) is empty", localX, localZ);
            return chunkData;
        }

        Log::Debug("Chunk location: offset=%u, sectors=%u", location.sectorOffset, location.sectorCount);

        // Open file stream
        auto& fileStream = regionFile.GetFileStream();
        if (!fileStream.is_open()) {
            Log::Error("Region file stream is not open");
            return chunkData;
        }

        // Seek to chunk data
        size_t chunkOffset = static_cast<size_t>(location.sectorOffset) * RegionFile::SECTOR_SIZE;
        Log::Debug("Seeking to chunk offset: %zu", chunkOffset);
        fileStream.seekg(chunkOffset);

        if (fileStream.fail()) {
            Log::Error("Failed to seek to chunk offset %zu", chunkOffset);
            return chunkData;
        }

        // Read chunk header (4 bytes length + 1 byte version)
        uint8_t header[5];
        fileStream.read(reinterpret_cast<char*>(header), 5);

        if (fileStream.gcount() != 5) {
            Log::Error("Failed to read chunk header for (%d, %d): got %lld bytes, expected 5",
                      localX, localZ, static_cast<long long>(fileStream.gcount()));
            return chunkData;
        }

        // Parse header (big-endian)
        chunkData.length = (static_cast<uint32_t>(header[0]) << 24) |
                          (static_cast<uint32_t>(header[1]) << 16) |
                          (static_cast<uint32_t>(header[2]) << 8) |
                          static_cast<uint32_t>(header[3]);
        chunkData.version = header[4];

        Log::Debug("Chunk header: length=%u, version=%u", chunkData.length, chunkData.version);

        // Validate length
        if (chunkData.length == 0) {
            Log::Warning("Chunk (%d, %d) has zero length", localX, localZ);
            return chunkData;
        }

        if (chunkData.length > 1024 * 1024) { // 1MB sanity check
            Log::Error("Chunk (%d, %d) has suspiciously large length: %u bytes",
                      localX, localZ, chunkData.length);
            return chunkData;
        }

        // Validate compression version
        if (chunkData.version != 1 && chunkData.version != 2 && chunkData.version != 3) {
            Log::Error("Chunk (%d, %d) has unknown compression version: %u",
                      localX, localZ, chunkData.version);
            return chunkData;
        }

        // Read compressed data (length includes version byte, so subtract 1)
        uint32_t dataLength = chunkData.length - 1;
        chunkData.compressedData.resize(dataLength);

        Log::Debug("Reading %u bytes of compressed data", dataLength);
        fileStream.read(reinterpret_cast<char*>(chunkData.compressedData.data()), dataLength);

        if (static_cast<uint32_t>(fileStream.gcount()) != dataLength) {
            Log::Error("Failed to read chunk data for (%d, %d): expected %u bytes, got %lld",
                      localX, localZ, dataLength, static_cast<long long>(fileStream.gcount()));
            return chunkData;
        }

        Log::Debug("Successfully read %u bytes of compressed data", dataLength);

        // Decompress data
        if (!DecompressChunkDataWithDebug(chunkData)) {
            Log::Error("Failed to decompress chunk data for (%d, %d)", localX, localZ);
            return chunkData;
        }

        Log::Debug("Decompressed to %zu bytes", chunkData.uncompressedData.size());

        // Log first few bytes of uncompressed data
        if (!chunkData.uncompressedData.empty()) {
            std::ostringstream hexDump;
            size_t dumpSize = std::min(chunkData.uncompressedData.size(), size_t(16));
            for (size_t i = 0; i < dumpSize; ++i) {
                hexDump << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(chunkData.uncompressedData[i]) << " ";
            }
            Log::Debug("First %zu bytes of NBT data: %s", dumpSize, hexDump.str().c_str());
        }

        // Parse NBT data - we'll use the regular parser for now
        // (Debug parser would require adding GetLevel() method to Log class)
        bool useDebugParser = false; // Set to true manually if you want debug parsing

        if (useDebugParser) {
            Log::Debug("Using debug NBT parser");
            chunkData.rootTag = NBTParser::ParseWithDebug(chunkData.uncompressedData);
        } else {
            chunkData.rootTag = NBTParser::Parse(chunkData.uncompressedData);
        }

        if (!chunkData.rootTag) {
            Log::Error("Failed to parse NBT data for chunk (%d, %d)", localX, localZ);

            // Additional debugging when NBT parsing fails
            if (!chunkData.uncompressedData.empty()) {
                Log::Debug("NBT parse failure analysis:");
                Log::Debug("  Data size: %zu bytes", chunkData.uncompressedData.size());
                Log::Debug("  First byte: 0x%02X (should be 0x0A for TAG_Compound)",
                          static_cast<unsigned>(chunkData.uncompressedData[0]));

                if (chunkData.uncompressedData.size() >= 3) {
                    uint16_t nameLength = (static_cast<uint16_t>(chunkData.uncompressedData[1]) << 8) |
                                         static_cast<uint16_t>(chunkData.uncompressedData[2]);
                    Log::Debug("  Root name length: %u", nameLength);

                    if (nameLength > 0 && nameLength < 100 && chunkData.uncompressedData.size() >= 3 + nameLength) {
                        std::string rootName(reinterpret_cast<const char*>(&chunkData.uncompressedData[3]), nameLength);
                        Log::Debug("  Root name: '%s'", rootName.c_str());
                    }
                }
            }

            return chunkData;
        }

        // Extract actual world coordinates from NBT
        auto compound = std::dynamic_pointer_cast<NBTTagCompound>(chunkData.rootTag);
        if (compound) {
            // Try to get Level compound (pre-1.18 format)
            auto levelTag = compound->GetTag("Level");
            if (levelTag) {
                auto levelCompound = std::dynamic_pointer_cast<NBTTagCompound>(levelTag);
                if (levelCompound) {
                    chunkData.worldX = levelCompound->GetValue<int32_t>("xPos", chunkData.worldX);
                    chunkData.worldZ = levelCompound->GetValue<int32_t>("zPos", chunkData.worldZ);

                    // Check for 1.18+ yPos
                    int32_t yPos = levelCompound->GetValue<int32_t>("yPos", INT32_MIN);
                    if (yPos != INT32_MIN) {
                        Log::Debug("Found 1.18+ yPos: %d", yPos);
                    }
                }
            } else {
                // 1.18+ format: coordinates might be at root level
                chunkData.worldX = compound->GetValue<int32_t>("xPos", chunkData.worldX);
                chunkData.worldZ = compound->GetValue<int32_t>("zPos", chunkData.worldZ);

                // Check DataVersion to confirm 1.18+
                int32_t dataVersion = compound->GetValue<int32_t>("DataVersion", -1);
                if (dataVersion >= 2825) { // 1.18 data version
                    Log::Debug("Detected 1.18+ chunk format (DataVersion: %d)", dataVersion);
                }
            }
        }

        chunkData.isValid = true;
        Log::Debug("Successfully parsed chunk (%d, %d) -> world (%d, %d)",
                  localX, localZ, chunkData.worldX, chunkData.worldZ);

        return chunkData;
    }

    bool RegionDumper::DecompressChunkDataWithDebug(ChunkData& chunkData) {
        if (chunkData.compressedData.empty()) {
            Log::Error("No compressed data to decompress");
            return false;
        }

        Log::Debug("Decompressing %zu bytes using version %u",
                  chunkData.compressedData.size(), chunkData.version);

        // Determine decompression method based on version
        const char* compressionName = "Unknown";
        switch (chunkData.version) {
            case 1: compressionName = "GZip"; break;
            case 2: compressionName = "ZLib"; break;
            case 3: compressionName = "Uncompressed"; break;
        }
        Log::Debug("Compression type: %s", compressionName);

        if (chunkData.version == 3) {
            // Uncompressed data
            chunkData.uncompressedData = chunkData.compressedData;
            Log::Debug("Data is uncompressed, copying %zu bytes", chunkData.uncompressedData.size());
            return true;
        }

        // Decompress using zlib (handles both GZip and ZLib)
        uLongf destLen = 1024 * 1024; // 1MB initial buffer
        chunkData.uncompressedData.resize(destLen);

        int result = uncompress(
            chunkData.uncompressedData.data(),
            &destLen,
            chunkData.compressedData.data(),
            chunkData.compressedData.size()
        );

        if (result == Z_OK) {
            chunkData.uncompressedData.resize(destLen);
            Log::Debug("Decompression successful: %zu -> %lu bytes (ratio: %.2f%%)",
                      chunkData.compressedData.size(), destLen,
                      (100.0 * chunkData.compressedData.size()) / destLen);
            return true;
        } else {
            const char* errorMsg = "Unknown error";
            switch (result) {
                case Z_MEM_ERROR: errorMsg = "Not enough memory"; break;
                case Z_BUF_ERROR: errorMsg = "Output buffer too small"; break;
                case Z_DATA_ERROR: errorMsg = "Input data corrupted"; break;
            }
            Log::Error("ZLib decompression failed: %s (code: %d)", errorMsg, result);

            // Try with a larger buffer for Z_BUF_ERROR
            if (result == Z_BUF_ERROR) {
                Log::Debug("Retrying with larger buffer (4MB)");
                destLen = 4 * 1024 * 1024; // 4MB buffer
                chunkData.uncompressedData.resize(destLen);

                result = uncompress(
                    chunkData.uncompressedData.data(),
                    &destLen,
                    chunkData.compressedData.data(),
                    chunkData.compressedData.size()
                );

                if (result == Z_OK) {
                    chunkData.uncompressedData.resize(destLen);
                    Log::Info("Decompression successful with larger buffer: %lu bytes", destLen);
                    return true;
                } else {
                    Log::Error("Decompression failed even with larger buffer (code: %d)", result);
                }
            }
        }

        return false;
    }

    // Enhanced validation for 1.18+ chunks
    bool RegionDumper::ValidateChunkNBT118Plus(const NBTTagPtr& rootTag) {
        if (!rootTag) {
            Log::Warning("Root tag is null");
            return false;
        }

        auto rootCompound = std::dynamic_pointer_cast<NBTTagCompound>(rootTag);
        if (!rootCompound) {
            Log::Warning("Root tag is not a compound");
            return false;
        }

        // Check DataVersion to determine format
        int32_t dataVersion = rootCompound->GetValue<int32_t>("DataVersion", -1);
        Log::Debug("Chunk DataVersion: %d", dataVersion);

        if (dataVersion >= 2825) { // 1.18+
            Log::Debug("Validating 1.18+ chunk format");

            // 1.18+ format validation
            bool hasXPos = rootCompound->HasTag("xPos");
            bool hasZPos = rootCompound->HasTag("zPos");
            bool hasYPos = rootCompound->HasTag("yPos");
            bool hasStatus = rootCompound->HasTag("Status");

            if (!hasXPos || !hasZPos) {
                Log::Warning("1.18+ chunk missing position tags (xPos/zPos)");
                return false;
            }

            if (hasYPos) {
                int32_t yPos = rootCompound->GetValue<int32_t>("yPos", 0);
                Log::Debug("1.18+ chunk yPos: %d", yPos);
            }

            if (hasStatus) {
                std::string status = rootCompound->GetValue<std::string>("Status", "unknown");
                Log::Debug("1.18+ chunk status: %s", status.c_str());
            }

            // Check for sections
            auto sectionsTag = rootCompound->GetTag("sections");
            if (sectionsTag) {
                auto sectionsList = std::dynamic_pointer_cast<NBTTagList>(sectionsTag);
                if (sectionsList) {
                    Log::Debug("1.18+ chunk has %zu sections", sectionsList->value.size());
                }
            }

        } else {
            Log::Debug("Validating pre-1.18 chunk format");

            // Pre-1.18 format validation
            bool hasLevel = rootCompound->HasTag("Level");
            if (!hasLevel) {
                Log::Warning("Pre-1.18 chunk missing 'Level' tag");
                return false;
            }

            auto levelTag = std::dynamic_pointer_cast<NBTTagCompound>(rootCompound->GetTag("Level"));
            if (!levelTag) {
                Log::Warning("'Level' tag is not a compound");
                return false;
            }

            bool hasXPos = levelTag->HasTag("xPos");
            bool hasZPos = levelTag->HasTag("zPos");

            if (!hasXPos || !hasZPos) {
                Log::Warning("Pre-1.18 chunk missing position tags (xPos/zPos) in Level");
                return false;
            }
        }

        Log::Debug("NBT validation passed - chunk has required structure");
        return true;
    }

} // namespace World