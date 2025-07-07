// File: src/world/RegionFile.cpp
#include "RegionFile.hpp"
#include "../core/Log.hpp"
#include <filesystem>
#include <cstring>

namespace World {

    RegionFile::RegionFile(const std::string& filePath)
        : filePath(filePath)
        , headerLoaded(false)
        , fileExists(false) {

        // Check if file exists
        fileExists = std::filesystem::exists(filePath);
        if (!fileExists) {
            Log::Warning("Region file does not exist: %s", filePath.c_str());
        }
    }

    bool RegionFile::LoadHeader() {
        if (headerLoaded) {
            return true; // Already loaded
        }

        if (!fileExists) {
            Log::Error("Cannot load header - file does not exist: %s", filePath.c_str());
            return false;
        }

        // Open file in binary mode
        fileStream.open(filePath, std::ios::binary);
        if (!fileStream.is_open()) {
            Log::Error("Failed to open region file: %s", filePath.c_str());
            return false;
        }

        // Get file size
        fileStream.seekg(0, std::ios::end);
        size_t fileSize = fileStream.tellg();
        fileStream.seekg(0, std::ios::beg);

        if (fileSize < HEADER_SIZE) {
            Log::Error("Region file too small (expected at least %d bytes, got %zu): %s",
                      HEADER_SIZE, fileSize, filePath.c_str());
            fileStream.close();
            return false;
        }

        Log::Debug("Loading region file header: %s (size: %zu bytes)", filePath.c_str(), fileSize);

        // Read location table (first 4KB)
        std::array<uint8_t, SECTOR_SIZE> locationBuffer;
        fileStream.read(reinterpret_cast<char*>(locationBuffer.data()), SECTOR_SIZE);

        if (fileStream.gcount() != SECTOR_SIZE) {
            Log::Error("Failed to read location table from region file: %s", filePath.c_str());
            fileStream.close();
            return false;
        }

        // Parse location table (1024 entries of 4 bytes each)
        for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
            // Each entry is 4 bytes: 3 bytes offset + 1 byte count (big-endian)
            uint32_t entry = 0;
            entry |= (static_cast<uint32_t>(locationBuffer[i * 4 + 0]) << 24);
            entry |= (static_cast<uint32_t>(locationBuffer[i * 4 + 1]) << 16);
            entry |= (static_cast<uint32_t>(locationBuffer[i * 4 + 2]) << 8);
            entry |= (static_cast<uint32_t>(locationBuffer[i * 4 + 3]) << 0);

            locationTable[i] = ParseLocationEntry(entry);
        }

        // Read timestamp table (second 4KB)
        std::array<uint8_t, SECTOR_SIZE> timestampBuffer;
        fileStream.read(reinterpret_cast<char*>(timestampBuffer.data()), SECTOR_SIZE);

        if (fileStream.gcount() != SECTOR_SIZE) {
            Log::Error("Failed to read timestamp table from region file: %s", filePath.c_str());
            fileStream.close();
            return false;
        }

        // Parse timestamp table (1024 entries of 4 bytes each, big-endian)
        for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
            uint32_t timestamp = 0;
            timestamp |= (static_cast<uint32_t>(timestampBuffer[i * 4 + 0]) << 24);
            timestamp |= (static_cast<uint32_t>(timestampBuffer[i * 4 + 1]) << 16);
            timestamp |= (static_cast<uint32_t>(timestampBuffer[i * 4 + 2]) << 8);
            timestamp |= (static_cast<uint32_t>(timestampBuffer[i * 4 + 3]) << 0);

            timestampTable[i] = timestamp;
        }

        headerLoaded = true;

        // Count non-empty chunks for statistics
        int nonEmptyChunks = 0;
        for (const auto& loc : locationTable) {
            if (!loc.isEmpty()) {
                nonEmptyChunks++;
            }
        }

        Log::Info("Region file header loaded successfully: %s (%d non-empty chunks)",
                 filePath.c_str(), nonEmptyChunks);

        return true;
    }

    ChunkLoc RegionFile::GetLocation(int localX, int localZ) const {
        if (localX < 0 || localX >= REGION_SIZE || localZ < 0 || localZ >= REGION_SIZE) {
            Log::Warning("Invalid local coordinates: (%d, %d)", localX, localZ);
            return ChunkLoc();
        }

        if (!headerLoaded) {
            Log::Warning("Region file header not loaded, cannot get location");
            return ChunkLoc();
        }

        int index = CoordinatesToIndex(localX, localZ);
        return locationTable[index];
    }

    uint32_t RegionFile::GetTimestamp(int localX, int localZ) const {
        if (localX < 0 || localX >= REGION_SIZE || localZ < 0 || localZ >= REGION_SIZE) {
            Log::Warning("Invalid local coordinates: (%d, %d)", localX, localZ);
            return 0;
        }

        if (!headerLoaded) {
            Log::Warning("Region file header not loaded, cannot get timestamp");
            return 0;
        }

        int index = CoordinatesToIndex(localX, localZ);
        return timestampTable[index];
    }

    int RegionFile::CoordinatesToIndex(int localX, int localZ) {
        // Minecraft region file layout: index = localX + localZ * 32
        return localX + localZ * REGION_SIZE;
    }

    ChunkLoc RegionFile::ParseLocationEntry(uint32_t entry) {
        // Entry format: [offset:24][count:8] (big-endian)
        uint32_t offset = (entry >> 8) & 0xFFFFFF;  // Upper 24 bits
        uint8_t count = entry & 0xFF;               // Lower 8 bits

        return ChunkLoc(offset, count);
    }

} // namespace World