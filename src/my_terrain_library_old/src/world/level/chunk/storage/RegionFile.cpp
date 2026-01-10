#include "world/level/chunk/storage/RegionFile.h"
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <zlib.h>
#include <filesystem>

// Reference: net/minecraft/world/level/chunk/storage/RegionFile.java

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

// Reference: RegionFile.java constructor lines 51-107
RegionFile::RegionFile(const RegionStorageInfo& info, const std::string& path,
                       const std::string& externalFileDir, bool sync)
    : m_info(info)
    , m_path(path)
    , m_externalFileDir(externalFileDir)
    , m_sync(sync)
    , m_offsets(SECTOR_INTS, 0)
    , m_timestamps(SECTOR_INTS, 0)
{
    // Create directory if needed
    std::filesystem::path filePath(path);
    std::filesystem::create_directories(filePath.parent_path());

    // Open or create file
    m_file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!m_file.is_open()) {
        // File doesn't exist, create it
        m_file.clear();
        m_file.open(path, std::ios::out | std::ios::binary);
        m_file.close();
        m_file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    }

    if (!m_file.is_open()) {
        throw std::runtime_error("Failed to open region file: " + path);
    }

    // Check file size
    m_file.seekg(0, std::ios::end);
    std::streamsize fileSize = m_file.tellg();
    m_file.seekg(0);

    if (fileSize < 2 * SECTOR_BYTES) {
        // Initialize new file with empty header
        writeHeader();
        // Pad to minimum size
        std::vector<char> padding(2 * SECTOR_BYTES - fileSize, 0);
        m_file.seekp(fileSize);
        m_file.write(padding.data(), padding.size());
        m_file.flush();
    } else {
        readHeader();
    }

    // Mark header sectors as used
    m_usedSectors.force(0, 2);

    // Mark all sectors containing chunk data as used
    for (int i = 0; i < SECTOR_INTS; ++i) {
        int32_t offset = m_offsets[i];
        if (offset != 0) {
            int sectorNumber = getSectorNumber(offset);
            int sectorCount = getNumSectors(offset);
            m_usedSectors.force(sectorNumber, sectorNumber + sectorCount);
        }
    }
}

RegionFile::~RegionFile() {
    close();
}

void RegionFile::readHeader() {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_file.seekg(0);

    // Read offsets (4 bytes each, big-endian)
    for (int i = 0; i < SECTOR_INTS; ++i) {
        uint8_t bytes[4];
        m_file.read(reinterpret_cast<char*>(bytes), 4);
        m_offsets[i] = (static_cast<int32_t>(bytes[0]) << 24) |
                       (static_cast<int32_t>(bytes[1]) << 16) |
                       (static_cast<int32_t>(bytes[2]) << 8) |
                       static_cast<int32_t>(bytes[3]);
    }

    // Read timestamps (4 bytes each, big-endian)
    for (int i = 0; i < SECTOR_INTS; ++i) {
        uint8_t bytes[4];
        m_file.read(reinterpret_cast<char*>(bytes), 4);
        m_timestamps[i] = (static_cast<int32_t>(bytes[0]) << 24) |
                          (static_cast<int32_t>(bytes[1]) << 16) |
                          (static_cast<int32_t>(bytes[2]) << 8) |
                          static_cast<int32_t>(bytes[3]);
    }
}

void RegionFile::writeHeader() {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_file.seekp(0);

    // Write offsets (big-endian)
    for (int i = 0; i < SECTOR_INTS; ++i) {
        int32_t offset = m_offsets[i];
        char bytes[4];
        bytes[0] = static_cast<char>((offset >> 24) & 0xFF);
        bytes[1] = static_cast<char>((offset >> 16) & 0xFF);
        bytes[2] = static_cast<char>((offset >> 8) & 0xFF);
        bytes[3] = static_cast<char>(offset & 0xFF);
        m_file.write(bytes, 4);
    }

    // Write timestamps (big-endian)
    for (int i = 0; i < SECTOR_INTS; ++i) {
        int32_t timestamp = m_timestamps[i];
        char bytes[4];
        bytes[0] = static_cast<char>((timestamp >> 24) & 0xFF);
        bytes[1] = static_cast<char>((timestamp >> 16) & 0xFF);
        bytes[2] = static_cast<char>((timestamp >> 8) & 0xFF);
        bytes[3] = static_cast<char>(timestamp & 0xFF);
        m_file.write(bytes, 4);
    }

    if (m_sync) {
        m_file.flush();
    }
}

// Reference: RegionFile.java hasChunk(ChunkPos)
bool RegionFile::hasChunk(const ChunkPos& pos) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    int index = getOffsetIndex(pos);
    return m_offsets[index] != 0;
}

// Reference: RegionFile.java getChunkDataInputStream(ChunkPos)
std::unique_ptr<std::istream> RegionFile::getChunkDataInputStream(const ChunkPos& pos) {
    std::vector<char> data = readChunkData(pos);
    if (data.empty()) {
        return nullptr;
    }

    // Create string stream from decompressed data
    auto stream = std::make_unique<std::istringstream>(
        std::string(data.begin(), data.end()), std::ios::binary);
    return stream;
}

std::vector<char> RegionFile::readChunkData(const ChunkPos& pos) {
    std::lock_guard<std::mutex> lock(m_mutex);

    int index = getOffsetIndex(pos);
    int32_t offset = m_offsets[index];

    if (offset == 0) {
        return {};
    }

    int sectorNumber = getSectorNumber(offset);
    int sectorCount = getNumSectors(offset);

    if (sectorCount == 0) {
        return {};
    }

    // Seek to sector
    m_file.seekg(static_cast<std::streamoff>(sectorNumber) * SECTOR_BYTES);

    // Read length (4 bytes, big-endian)
    uint8_t lengthBytes[4];
    m_file.read(reinterpret_cast<char*>(lengthBytes), 4);
    int32_t length = (static_cast<int32_t>(lengthBytes[0]) << 24) |
                     (static_cast<int32_t>(lengthBytes[1]) << 16) |
                     (static_cast<int32_t>(lengthBytes[2]) << 8) |
                     static_cast<int32_t>(lengthBytes[3]);

    if (length <= 0 || length > sectorCount * SECTOR_BYTES - 4) {
        return {};
    }

    // Read compression type (1 byte)
    uint8_t compressionType;
    m_file.read(reinterpret_cast<char*>(&compressionType), 1);

    // Read compressed data
    std::vector<char> compressedData(length - 1);
    m_file.read(compressedData.data(), length - 1);

    // Decompress based on type
    if (compressionType == COMPRESSION_NONE) {
        return compressedData;
    }

    // Decompress using zlib (both GZIP and ZLIB use same decompression)
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(compressedData.data());
    strm.avail_in = static_cast<uInt>(compressedData.size());

    int windowBits = (compressionType == COMPRESSION_GZIP) ? (16 + MAX_WBITS) : MAX_WBITS;
    if (inflateInit2(&strm, windowBits) != Z_OK) {
        return {};
    }

    std::vector<char> decompressedData;
    char buffer[16384];

    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return {};
        }

        size_t have = sizeof(buffer) - strm.avail_out;
        decompressedData.insert(decompressedData.end(), buffer, buffer + have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return decompressedData;
}

// Reference: RegionFile.java getChunkDataOutputStream(ChunkPos)
std::unique_ptr<RegionFile::ChunkDataOutputStream> RegionFile::getChunkDataOutputStream(const ChunkPos& pos) {
    return std::make_unique<ChunkDataOutputStream>(this, pos);
}

void RegionFile::ChunkDataOutputStream::finalize() {
    if (!m_parent) return;

    std::string uncompressedData = str();
    if (uncompressedData.empty()) {
        m_parent = nullptr;
        return;
    }

    // Compress using zlib
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(uncompressedData.data()));
    strm.avail_in = static_cast<uInt>(uncompressedData.size());

    if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
        m_parent = nullptr;
        return;
    }

    std::vector<char> compressedData;
    char buffer[16384];

    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        strm.avail_out = sizeof(buffer);

        ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&strm);
            m_parent = nullptr;
            return;
        }

        size_t have = sizeof(buffer) - strm.avail_out;
        compressedData.insert(compressedData.end(), buffer, buffer + have);
    } while (strm.avail_out == 0);

    deflateEnd(&strm);

    m_parent->writeChunk(m_pos, compressedData);
    m_parent = nullptr;
}

void RegionFile::writeChunk(const ChunkPos& pos, const std::vector<char>& compressedData) {
    std::lock_guard<std::mutex> lock(m_mutex);

    int index = getOffsetIndex(pos);
    int32_t oldOffset = m_offsets[index];

    // Free old sectors
    if (oldOffset != 0) {
        int oldSectorNumber = getSectorNumber(oldOffset);
        int oldSectorCount = getNumSectors(oldOffset);
        m_usedSectors.free(oldSectorNumber, oldSectorNumber + oldSectorCount);
    }

    // Calculate required sectors
    int32_t totalSize = static_cast<int32_t>(compressedData.size()) + CHUNK_HEADER_SIZE;
    int requiredSectors = (totalSize + SECTOR_BYTES - 1) / SECTOR_BYTES;

    if (requiredSectors > 255) {
        // Chunk too large - would need external file
        // For now, just skip it
        m_offsets[index] = 0;
        return;
    }

    // Allocate sectors
    int sectorNumber = m_usedSectors.allocate(requiredSectors);

    // Seek to sector
    m_file.seekp(static_cast<std::streamoff>(sectorNumber) * SECTOR_BYTES);

    // Write length (4 bytes, big-endian) - includes compression type byte
    int32_t length = static_cast<int32_t>(compressedData.size()) + 1;
    char lengthBytes[4];
    lengthBytes[0] = static_cast<char>((length >> 24) & 0xFF);
    lengthBytes[1] = static_cast<char>((length >> 16) & 0xFF);
    lengthBytes[2] = static_cast<char>((length >> 8) & 0xFF);
    lengthBytes[3] = static_cast<char>(length & 0xFF);
    m_file.write(lengthBytes, 4);

    // Write compression type
    char compressionType = COMPRESSION_ZLIB;
    m_file.write(&compressionType, 1);

    // Write compressed data
    m_file.write(compressedData.data(), compressedData.size());

    // Pad to sector boundary
    int padding = requiredSectors * SECTOR_BYTES - totalSize;
    if (padding > 0) {
        std::vector<char> paddingData(padding, 0);
        m_file.write(paddingData.data(), padding);
    }

    // Update offset
    m_offsets[index] = packSectorOffset(sectorNumber, requiredSectors);

    // Update timestamp
    m_timestamps[index] = static_cast<int32_t>(std::time(nullptr));

    // Write updated header entry
    m_file.seekp(index * 4);
    char offsetBytes[4];
    offsetBytes[0] = static_cast<char>((m_offsets[index] >> 24) & 0xFF);
    offsetBytes[1] = static_cast<char>((m_offsets[index] >> 16) & 0xFF);
    offsetBytes[2] = static_cast<char>((m_offsets[index] >> 8) & 0xFF);
    offsetBytes[3] = static_cast<char>(m_offsets[index] & 0xFF);
    m_file.write(offsetBytes, 4);

    m_file.seekp(SECTOR_BYTES + index * 4);
    char timestampBytes[4];
    timestampBytes[0] = static_cast<char>((m_timestamps[index] >> 24) & 0xFF);
    timestampBytes[1] = static_cast<char>((m_timestamps[index] >> 16) & 0xFF);
    timestampBytes[2] = static_cast<char>((m_timestamps[index] >> 8) & 0xFF);
    timestampBytes[3] = static_cast<char>(m_timestamps[index] & 0xFF);
    m_file.write(timestampBytes, 4);

    if (m_sync) {
        m_file.flush();
    }
}

void RegionFile::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_file.flush();
}

void RegionFile::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.close();
    }
}

std::string RegionFile::getRegionFileName(int regionX, int regionZ) {
    return "r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca";
}

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
