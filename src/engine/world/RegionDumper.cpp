// File: src/tools/RegionDumper.cpp
#include "RegionDumper.hpp"
#include "RegionFileCache.hpp"
#include "../../core/Log.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <ctime>
#include <zlib.h>

namespace World {

    // **NEW**: Thread-safe file access with per-thread file handles
    static std::mutex s_fileAccessMutex;

    // Proper inflate function that handles both zlib and gzip
    bool InflateAll(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        if (in.empty()) {
            Log::Error("Empty input data for inflation");
            return false;
        }

        z_stream strm{};
        strm.next_in = (Bytef*)in.data();
        strm.avail_in = in.size();

        // 15 window bits + 32 to auto-detect zlib or gzip header
        int ret = inflateInit2(&strm, 15 + 32);
        if (ret != Z_OK) {
            Log::Error("inflateInit2 failed: %d", ret);
            return false;
        }

        out.clear();
        out.resize(in.size() * 4); // Start with 4x the compressed size
        strm.next_out = (Bytef*)out.data();
        strm.avail_out = out.size();

        while (true) {
            ret = inflate(&strm, Z_NO_FLUSH);

            if (ret == Z_STREAM_END) {
                // Successfully decompressed
                break;
            } else if (ret == Z_BUF_ERROR || ret == Z_OK) {
                // Need more output buffer space
                size_t used = strm.total_out;
                out.resize(out.size() * 2);
                strm.next_out = (Bytef*)out.data() + used;
                strm.avail_out = out.size() - used;
                continue;
            } else {
                // Error occurred
                const char* errorMsg = "Unknown error";
                switch (ret) {
                    case Z_STREAM_ERROR: errorMsg = "Stream error"; break;
                    case Z_DATA_ERROR: errorMsg = "Data error"; break;
                    case Z_MEM_ERROR: errorMsg = "Memory error"; break;
                    case Z_VERSION_ERROR: errorMsg = "Version error"; break;
                }
                Log::Error("inflate failed: %s (code: %d)", errorMsg, ret);
                inflateEnd(&strm);
                return false;
            }
        }

        size_t finalSize = strm.total_out;
        inflateEnd(&strm);
        out.resize(finalSize);

        Log::Debug("Decompression successful: %zu -> %zu bytes (ratio: %.2f%%)",
                  in.size(), finalSize, (100.0 * in.size()) / finalSize);
        return true;
    }

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

        // **CRITICAL FIX**: Create a NEW file stream for this operation instead of reusing
        // the potentially shared one from RegionFile
        std::string filePath = regionFile.GetFilePath();

        // **THREAD SAFETY**: Use a separate file handle per read operation
        std::ifstream localFileStream;

        {
            // **MUTEX PROTECTION**: Protect file opening and seeking
            std::lock_guard<std::mutex> lock(s_fileAccessMutex);

            localFileStream.open(filePath, std::ios::binary);
            if (!localFileStream.is_open()) {
                Log::Error("Failed to open region file for chunk reading: %s", filePath.c_str());
                return chunkData;
            }

            // Seek to chunk data
            size_t chunkOffset = static_cast<size_t>(location.sectorOffset) * RegionFile::SECTOR_SIZE;
            localFileStream.seekg(chunkOffset);

            if (localFileStream.fail()) {
                Log::Error("Failed to seek to chunk offset %zu in %s", chunkOffset, filePath.c_str());
                return chunkData;
            }
        } // End of mutex protection for seek operations

        // **NO MUTEX**: File reading can happen without mutex since each thread has its own stream

        // Read chunk header (4 bytes length + 1 byte version)
        uint8_t header[5];
        localFileStream.read(reinterpret_cast<char*>(header), 5);

        if (localFileStream.gcount() != 5) {
            Log::Error("Failed to read chunk header for (%d, %d): expected 5 bytes, got %lld",
                      localX, localZ, static_cast<long long>(localFileStream.gcount()));
            return chunkData;
        }

        // Parse header (big-endian)
        chunkData.length = (static_cast<uint32_t>(header[0]) << 24) |
                          (static_cast<uint32_t>(header[1]) << 16) |
                          (static_cast<uint32_t>(header[2]) << 8) |
                          static_cast<uint32_t>(header[3]);
        chunkData.version = header[4];

        Log::Debug("Chunk (%d, %d): length=%u, version=%u", localX, localZ, chunkData.length, chunkData.version);

        // Validate length
        if (chunkData.length == 0 || chunkData.length > 1024 * 1024) { // 1MB sanity check
            Log::Error("Invalid chunk length: %u for chunk (%d, %d)", chunkData.length, localX, localZ);
            return chunkData;
        }

        // Validate compression version
        if (chunkData.version != 1 && chunkData.version != 2 && chunkData.version != 3) {
            Log::Error("Unknown compression version: %u for chunk (%d, %d)", chunkData.version, localX, localZ);
            return chunkData;
        }

        // Read compressed data (length includes version byte, so subtract 1)
        uint32_t dataLength = chunkData.length - 1;

        // **CRITICAL**: Validate data length before allocation
        if (dataLength == 0) {
            Log::Error("Zero data length after header for chunk (%d, %d)", localX, localZ);
            return chunkData;
        }

        if (dataLength > 10 * 1024 * 1024) { // 10MB sanity check
            Log::Error("Suspiciously large data length %u for chunk (%d, %d)", dataLength, localX, localZ);
            return chunkData;
        }

        chunkData.compressedData.resize(dataLength);

        // Check stream state before reading
        if (!localFileStream.good()) {
            Log::Error("File stream is in bad state before reading chunk data for (%d, %d)", localX, localZ);
            return chunkData;
        }

        localFileStream.read(reinterpret_cast<char*>(chunkData.compressedData.data()), dataLength);

        // **ENHANCED**: Check both bytes read AND stream state
        std::streamsize bytesRead = localFileStream.gcount();
        if (bytesRead != static_cast<std::streamsize>(dataLength)) {
            Log::Error("Failed to read chunk data for (%d, %d): expected %u bytes, got %lld, stream state: good=%d, eof=%d, fail=%d, bad=%d",
                      localX, localZ, dataLength, static_cast<long long>(bytesRead),
                      localFileStream.good(), localFileStream.eof(), localFileStream.fail(), localFileStream.bad());
            return chunkData;
        }

        // **SAFETY**: Close the local file stream
        localFileStream.close();

        Log::Debug("Successfully read %u bytes of compressed data for chunk (%d, %d)", dataLength, localX, localZ);

        // Decompress data using the improved inflate function
        if (!DecompressChunkData(chunkData)) {
            Log::Error("Failed to decompress chunk data for (%d, %d)", localX, localZ);
            return chunkData;
        }

        // Debug: Print first 16 bytes of uncompressed data (only if enabled)
        #ifdef DEBUG_NBT_READING
        if (!chunkData.uncompressedData.empty()) {
            std::cout << "First 16 bytes of uncompressed NBT for chunk (" << localX << "," << localZ << "):";
            for (size_t i = 0; i < 16 && i < chunkData.uncompressedData.size(); ++i) {
                printf(" %02X", chunkData.uncompressedData[i]);
            }
            std::cout << std::endl;
        }
        #endif

        // Parse NBT data
        chunkData.rootTag = NBTParser::Parse(chunkData.uncompressedData);
        if (!chunkData.rootTag) {
            Log::Error("Failed to parse NBT data for chunk (%d, %d)", localX, localZ);
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
                }
            } else {
                // 1.18+ format: coordinates might be at root level
                chunkData.worldX = compound->GetValue<int32_t>("xPos", chunkData.worldX);
                chunkData.worldZ = compound->GetValue<int32_t>("zPos", chunkData.worldZ);
            }
        }

        chunkData.isValid = true;
        Log::Debug("Successfully processed chunk (%d, %d) with %zu bytes uncompressed data",
                  localX, localZ, chunkData.uncompressedData.size());

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

        Log::Debug("Decompressing %zu bytes using compression version %u",
                  chunkData.compressedData.size(), chunkData.version);

        try {
            if (chunkData.version == 3) {
                // Uncompressed data
                chunkData.uncompressedData = chunkData.compressedData;
                Log::Debug("Data is uncompressed, copying %zu bytes", chunkData.uncompressedData.size());
                return true;
            }

            // Use the improved inflate function for versions 1 (GZip) and 2 (ZLib)
            bool success = InflateAll(chunkData.compressedData, chunkData.uncompressedData);

            if (!success) {
                Log::Error("Decompression failed for compression version %u", chunkData.version);
                return false;
            }

            // Validate decompressed data
            if (chunkData.uncompressedData.empty()) {
                Log::Error("Decompression resulted in empty data");
                return false;
            }

            return true;

        } catch (const std::exception& e) {
            Log::Error("Exception during decompression: %s", e.what());
            return false;
        } catch (...) {
            Log::Error("Unknown exception during decompression");
            return false;
        }
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

        // For 1.18+ chunks, the data might be at root level
        if (!hasLevel) {
            // Check if this is a 1.18+ chunk with position data at root
            bool hasXPos = rootCompound->HasTag("xPos");
            bool hasZPos = rootCompound->HasTag("zPos");
            if (hasXPos && hasZPos) {
                Log::Debug("NBT validation passed - 1.18+ chunk format detected");
                return true;
            }

            Log::Warning("Chunk missing 'Level' tag and not 1.18+ format");
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

        if (!hasXPos || !hasZPos) {
            Log::Warning("Chunk missing position tags (xPos/zPos)");
            return false;
        }

        Log::Debug("NBT validation passed - pre-1.18 chunk has required structure");
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
            std::cout << "No Level data found (might be 1.18+ format)" << std::endl;

            // Try 1.18+ format where data is at root level
            int32_t xPos = rootCompound->GetValue<int32_t>("xPos", INT32_MIN);
            int32_t zPos = rootCompound->GetValue<int32_t>("zPos", INT32_MIN);
            if (xPos != INT32_MIN && zPos != INT32_MIN) {
                std::cout << "Chunk Position: (" << xPos << ", " << zPos << ") [1.18+ format]" << std::endl;

                int32_t yPos = rootCompound->GetValue<int32_t>("yPos", INT32_MIN);
                if (yPos != INT32_MIN) {
                    std::cout << "Y Position: " << yPos << std::endl;
                }

                std::string status = rootCompound->GetValue<std::string>("Status", "unknown");
                std::cout << "Generation Status: " << status << std::endl;
            }
            return;
        }

        // Position
        int32_t xPos = levelTag->GetValue<int32_t>("xPos", 0);
        int32_t zPos = levelTag->GetValue<int32_t>("zPos", 0);
        std::cout << "Chunk Position: (" << xPos << ", " << zPos << ") [pre-1.18 format]" << std::endl;

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
        // For now, just call the regular ReadChunkData which now has debug output
        return ReadChunkData(regionFile, localX, localZ);
    }

    bool RegionDumper::DecompressChunkDataWithDebug(ChunkData& chunkData) {
        // For now, just call the regular DecompressChunkData which now uses proper inflation
        return DecompressChunkData(chunkData);
    }

    bool RegionDumper::ValidateChunkNBT118Plus(const NBTTagPtr& rootTag) {
        // Enhanced validation that specifically handles 1.18+ format
        return ValidateChunkNBT(rootTag); // The regular validation now handles both formats
    }

} // namespace World