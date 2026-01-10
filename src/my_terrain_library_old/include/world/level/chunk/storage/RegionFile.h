#pragma once

#include "world/ChunkPos.h"
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <bitset>

// Reference: net/minecraft/world/level/chunk/storage/RegionFile.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

/**
 * RegionStorageInfo - Information about a region file
 * Reference: RegionStorageInfo.java
 */
struct RegionStorageInfo {
    std::string levelId;
    std::string dimension;
    std::string type;

    RegionStorageInfo() = default;
    RegionStorageInfo(const std::string& levelId, const std::string& dimension, const std::string& type)
        : levelId(levelId), dimension(dimension), type(type) {}
};

/**
 * RegionBitmap - Tracks used sectors in a region file
 * Reference: RegionBitmap.java
 */
class RegionBitmap {
public:
    void force(int from, int to) {
        for (int i = from; i < to; ++i) {
            m_used.set(i);
        }
    }

    void free(int from, int to) {
        for (int i = from; i < to; ++i) {
            m_used.reset(i);
        }
    }

    int allocate(int sectors) {
        int consecutive = 0;
        int startPos = 2;  // First two sectors are header

        for (size_t i = startPos; i < m_used.size(); ++i) {
            if (!m_used.test(i)) {
                consecutive++;
                if (consecutive >= sectors) {
                    int start = static_cast<int>(i) - sectors + 1;
                    force(start, start + sectors);
                    return start;
                }
            } else {
                consecutive = 0;
            }
        }

        // Need to extend file - allocate at end
        int start = static_cast<int>(m_used.count() > 0 ? m_used.size() : 2);
        force(start, start + sectors);
        return start;
    }

private:
    std::bitset<8192> m_used;  // Support up to 8192 sectors (32MB file)
};

/**
 * RegionFile - Manages a 32x32 chunk region file
 * Reference: RegionFile.java
 *
 * File format:
 * - Bytes 0-4095: Chunk locations (1024 entries, 4 bytes each)
 * - Bytes 4096-8191: Chunk timestamps (1024 entries, 4 bytes each)
 * - Bytes 8192+: Chunk data in 4096-byte sectors
 *
 * Chunk location format: (sector_number << 8) | sector_count
 * Chunk data format: length (4 bytes) + compression_type (1 byte) + compressed_data
 */
class RegionFile {
public:
    // Reference: RegionFile.java constants
    static constexpr int SECTOR_BYTES = 4096;
    static constexpr int SECTOR_INTS = 1024;  // 32x32 chunks
    static constexpr int CHUNK_HEADER_SIZE = 5;
    static constexpr int MAX_CHUNK_SIZE = 1024 * 1024;  // 1MB

    // Compression types
    static constexpr int COMPRESSION_GZIP = 1;
    static constexpr int COMPRESSION_ZLIB = 2;
    static constexpr int COMPRESSION_NONE = 3;

    // Reference: RegionFile.java constructor lines 51-107
    RegionFile(const RegionStorageInfo& info, const std::string& path,
               const std::string& externalFileDir, bool sync);

    ~RegionFile();

    // Reference: RegionFile.java getChunkDataInputStream(ChunkPos)
    std::unique_ptr<std::istream> getChunkDataInputStream(const ChunkPos& pos);

    // Reference: RegionFile.java getChunkDataOutputStream(ChunkPos)
    class ChunkDataOutputStream;
    std::unique_ptr<ChunkDataOutputStream> getChunkDataOutputStream(const ChunkPos& pos);

    // Reference: RegionFile.java hasChunk(ChunkPos)
    bool hasChunk(const ChunkPos& pos) const;

    // Reference: RegionFile.java flush()
    void flush();

    // Reference: RegionFile.java close()
    void close();

    // Get region coordinates from chunk position
    static int getRegionX(int chunkX) { return chunkX >> 5; }
    static int getRegionZ(int chunkZ) { return chunkZ >> 5; }

    // Get path for region file
    static std::string getRegionFileName(int regionX, int regionZ);

    /**
     * ChunkDataOutputStream - Buffered output stream that writes to region file on close
     */
    class ChunkDataOutputStream : public std::ostringstream {
    public:
        ChunkDataOutputStream(RegionFile* parent, const ChunkPos& pos)
            : std::ostringstream(std::ios::binary)
            , m_parent(parent)
            , m_pos(pos) {}

        ~ChunkDataOutputStream() {
            if (m_parent) {
                finalize();
            }
        }

        void finalize();

    private:
        RegionFile* m_parent;
        ChunkPos m_pos;
    };

private:
    RegionStorageInfo m_info;
    std::string m_path;
    std::string m_externalFileDir;
    bool m_sync;

    mutable std::mutex m_mutex;
    std::fstream m_file;

    // Header data
    std::vector<int32_t> m_offsets;     // 1024 entries
    std::vector<int32_t> m_timestamps;  // 1024 entries

    RegionBitmap m_usedSectors;

    // Reference: RegionFile.java getOffsetIndex(ChunkPos)
    static int getOffsetIndex(const ChunkPos& pos) {
        return (pos.x() & 31) + (pos.z() & 31) * 32;
    }

    // Reference: RegionFile.java packSectorOffset(int, int)
    static int32_t packSectorOffset(int sectorNumber, int sectorCount) {
        return (sectorNumber << 8) | (sectorCount & 0xFF);
    }

    // Reference: RegionFile.java getSectorNumber(int)
    static int getSectorNumber(int32_t offset) {
        return offset >> 8;
    }

    // Reference: RegionFile.java getNumSectors(int)
    static int getNumSectors(int32_t offset) {
        return offset & 0xFF;
    }

    void readHeader();
    void writeHeader();
    void writeChunk(const ChunkPos& pos, const std::vector<char>& data);
    std::vector<char> readChunkData(const ChunkPos& pos);
};

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
