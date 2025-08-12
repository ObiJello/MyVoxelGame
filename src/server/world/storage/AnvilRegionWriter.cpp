// File: src/server/world/storage/AnvilRegionWriter.cpp
#include "AnvilRegionWriter.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/core/Config.hpp"
#include "common/core/Log.hpp"
#include <filesystem>
#include <zlib.h>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace Game {

    // NBT Tag types (Minecraft specification)
    enum class NBTType : uint8_t {
        TAG_End = 0,
        TAG_Byte = 1,
        TAG_Short = 2,
        TAG_Int = 3,
        TAG_Long = 4,
        TAG_Float = 5,
        TAG_Double = 6,
        TAG_Byte_Array = 7,
        TAG_String = 8,
        TAG_List = 9,
        TAG_Compound = 10,
        TAG_Int_Array = 11,
        TAG_Long_Array = 12
    };

    AnvilRegionWriter::AnvilRegionWriter(const std::string& filePath) 
        : m_filePath(filePath) {
        // Initialize arrays
        m_locations.fill(AnvilChunkLocation{});
        m_timestamps.fill(0);
    }

    AnvilRegionWriter::~AnvilRegionWriter() {
        if (m_initialized) {
            Finalize();
        }
    }

    bool AnvilRegionWriter::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Debug("Initializing Anvil region writer: %s", m_filePath.c_str());

        // Ensure directory exists
        std::filesystem::path dirPath = std::filesystem::path(m_filePath).parent_path();
        if (!dirPath.empty() && !std::filesystem::exists(dirPath)) {
            if (!std::filesystem::create_directories(dirPath)) {
                Log::Error("Failed to create directory: %s", dirPath.string().c_str());
                return false;
            }
        }

        // Open file for read/write, create if doesn't exist
        bool fileExists = std::filesystem::exists(m_filePath);
        
        if (fileExists) {
            m_fileStream.open(m_filePath, std::ios::binary | std::ios::in | std::ios::out);
        } else {
            m_fileStream.open(m_filePath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        }

        if (!m_fileStream.is_open()) {
            Log::Error("Failed to open region file: %s", m_filePath.c_str());
            return false;
        }

        // Load or create header
        if (!LoadOrCreateHeader()) {
            Log::Error("Failed to load/create region file header");
            return false;
        }

        m_initialized = true;
        Log::Info("Anvil region writer initialized: %s", m_filePath.c_str());
        return true;
    }

    bool AnvilRegionWriter::WriteChunk(int localX, int localZ, const Chunk& chunk) {
        if (!m_initialized || !IsValidLocalCoords(localX, localZ)) {
            return false;
        }

        Log::Debug("Writing chunk (%d, %d) to region file", localX, localZ);

        // Serialize chunk to NBT
        std::vector<uint8_t> nbtData = SerializeChunkToNBT(chunk);
        if (nbtData.empty()) {
            Log::Error("Failed to serialize chunk to NBT");
            return false;
        }

        // Compress with Zlib
        std::vector<uint8_t> compressedData = CompressZlib(nbtData);
        if (compressedData.empty()) {
            Log::Error("Failed to compress chunk data");
            return false;
        }

        // Create final payload: [length:4][type:1][compressed_data]
        std::vector<uint8_t> payload;
        uint32_t totalLength = static_cast<uint32_t>(compressedData.size() + 1); // +1 for type byte

        // Write length (big-endian)
        payload.push_back((totalLength >> 24) & 0xFF);
        payload.push_back((totalLength >> 16) & 0xFF);
        payload.push_back((totalLength >> 8) & 0xFF);
        payload.push_back(totalLength & 0xFF);

        // Write compression type (2 = Zlib)
        payload.push_back(2);

        // Write compressed data
        payload.insert(payload.end(), compressedData.begin(), compressedData.end());

        // Calculate sectors needed
        int totalSize = static_cast<int>(payload.size());
        int sectorsNeeded = (totalSize + SECTOR_SIZE - 1) / SECTOR_SIZE; // Ceiling division

        int index = CoordsToIndex(localX, localZ);

        // Free old sectors if chunk already exists
        if (!m_locations[index].isEmpty()) {
            MarkSectorsFree(m_locations[index].sectorOffset, m_locations[index].sectorCount);
        }

        // Allocate new sectors
        int newOffset = AllocateSectors(sectorsNeeded);
        if (newOffset == -1) {
            Log::Error("Failed to allocate sectors for chunk (%d, %d)", localX, localZ);
            return false;
        }

        // Update header
        m_locations[index] = AnvilChunkLocation(newOffset, sectorsNeeded);
        m_timestamps[index] = GetCurrentTimestamp();

        // Write payload to file
        m_fileStream.seekp(static_cast<size_t>(newOffset) * SECTOR_SIZE);
        m_fileStream.write(reinterpret_cast<const char*>(payload.data()), totalSize);

        // Pad to sector boundary
        int padding = (sectorsNeeded * SECTOR_SIZE) - totalSize;
        if (padding > 0) {
            std::vector<char> zeros(padding, 0);
            m_fileStream.write(zeros.data(), padding);
        }

        if (!m_fileStream.good()) {
            Log::Error("Failed to write chunk data to file");
            return false;
        }

        // Save updated header
        if (!SaveHeader()) {
            Log::Error("Failed to save updated header");
            return false;
        }

        Log::Debug("Successfully wrote chunk (%d, %d) using %d sectors", 
                   localX, localZ, sectorsNeeded);
        return true;
    }

    bool AnvilRegionWriter::Finalize() {
        if (!m_initialized) {
            return true;
        }

        Log::Debug("Finalizing region file: %s", m_filePath.c_str());

        // Save final header
        bool success = SaveHeader();

        // Close file
        if (m_fileStream.is_open()) {
            m_fileStream.close();
        }

        m_initialized = false;
        return success;
    }

    void AnvilRegionWriter::ChunkToRegion(Math::ChunkPos chunkPos, int& regionX, int& regionZ, 
                                         int& localX, int& localZ) {
        regionX = chunkPos.x >> 5; // Divide by 32
        regionZ = chunkPos.z >> 5;
        
        // Handle negative coordinates properly
        if (chunkPos.x < 0 && (chunkPos.x & 31) != 0) regionX--;
        if (chunkPos.z < 0 && (chunkPos.z & 31) != 0) regionZ--;
        
        localX = chunkPos.x - (regionX << 5); // chunkX - regionX * 32
        localZ = chunkPos.z - (regionZ << 5); // chunkZ - regionZ * 32
    }

    std::string AnvilRegionWriter::GenerateRegionFilePath(const std::string& worldPath, 
                                                         int regionX, int regionZ) {
        std::filesystem::path path(worldPath);
        path /= "region";
        path /= ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".mca");
        return path.string();
    }

    // === PRIVATE IMPLEMENTATION ===

    bool AnvilRegionWriter::LoadOrCreateHeader() {
        // Check if file has existing data
        m_fileStream.seekg(0, std::ios::end);
        size_t fileSize = m_fileStream.tellg();
        m_fileStream.seekg(0, std::ios::beg);

        if (fileSize >= HEADER_SIZE) {
            // Load existing header
            Log::Debug("Loading existing region file header");

            // Read location table
            for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
                uint32_t packed;
                m_fileStream.read(reinterpret_cast<char*>(&packed), 4);
                
                // Convert from big-endian
                packed = ((packed & 0xFF) << 24) | 
                        (((packed >> 8) & 0xFF) << 16) | 
                        (((packed >> 16) & 0xFF) << 8) | 
                        ((packed >> 24) & 0xFF);
                
                m_locations[i] = AnvilChunkLocation::unpack(packed);
            }

            // Read timestamp table
            for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
                uint32_t timestamp;
                m_fileStream.read(reinterpret_cast<char*>(&timestamp), 4);
                
                // Convert from big-endian
                m_timestamps[i] = ((timestamp & 0xFF) << 24) | 
                                 (((timestamp >> 8) & 0xFF) << 16) | 
                                 (((timestamp >> 16) & 0xFF) << 8) | 
                                 ((timestamp >> 24) & 0xFF);
            }

            BuildSectorMap();
        } else {
            // Create new header
            Log::Debug("Creating new region file header");

            // Clear arrays (already done in constructor)
            // Initialize sector map (first 2 sectors are reserved for header)
            m_freeSectors.resize((fileSize + SECTOR_SIZE - 1) / SECTOR_SIZE, true);
            if (m_freeSectors.size() < 2) {
                m_freeSectors.resize(2);
            }
            m_freeSectors[0] = false; // Header sector 1
            m_freeSectors[1] = false; // Header sector 2

            // Write initial empty header
            if (!SaveHeader()) {
                return false;
            }
        }

        return true;
    }

    bool AnvilRegionWriter::SaveHeader() {
        m_fileStream.seekp(0);

        // Write location table (big-endian)
        for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
            uint32_t packed = m_locations[i].pack();
            
            // Convert to big-endian
            uint32_t be = ((packed & 0xFF) << 24) | 
                         (((packed >> 8) & 0xFF) << 16) | 
                         (((packed >> 16) & 0xFF) << 8) | 
                         ((packed >> 24) & 0xFF);
            
            m_fileStream.write(reinterpret_cast<const char*>(&be), 4);
        }

        // Write timestamp table (big-endian)
        for (int i = 0; i < CHUNKS_PER_REGION; ++i) {
            uint32_t timestamp = m_timestamps[i];
            
            // Convert to big-endian
            uint32_t be = ((timestamp & 0xFF) << 24) | 
                         (((timestamp >> 8) & 0xFF) << 16) | 
                         (((timestamp >> 16) & 0xFF) << 8) | 
                         ((timestamp >> 24) & 0xFF);
            
            m_fileStream.write(reinterpret_cast<const char*>(&be), 4);
        }

        m_fileStream.flush();
        return m_fileStream.good();
    }

    int AnvilRegionWriter::AllocateSectors(int sectorsNeeded) {
        if (sectorsNeeded <= 0) return -1;

        // Extend free sectors if needed
        size_t minSize = m_freeSectors.size();
        m_fileStream.seekg(0, std::ios::end);
        size_t fileSize = m_fileStream.tellg();
        size_t fileSectors = (fileSize + SECTOR_SIZE - 1) / SECTOR_SIZE;
        
        if (fileSectors > m_freeSectors.size()) {
            m_freeSectors.resize(fileSectors, true);
        }

        // Find contiguous free sectors
        for (size_t start = 2; start + sectorsNeeded <= m_freeSectors.size(); ++start) {
            bool canAllocate = true;
            for (int i = 0; i < sectorsNeeded; ++i) {
                if (!m_freeSectors[start + i]) {
                    canAllocate = false;
                    break;
                }
            }

            if (canAllocate) {
                // Mark sectors as used
                for (int i = 0; i < sectorsNeeded; ++i) {
                    m_freeSectors[start + i] = false;
                }
                return static_cast<int>(start);
            }
        }

        // No space found, extend file
        size_t newStart = m_freeSectors.size();
        m_freeSectors.resize(newStart + sectorsNeeded, false);
        
        // Zero out the new sectors
        m_fileStream.seekp(newStart * SECTOR_SIZE);
        std::vector<char> zeros(sectorsNeeded * SECTOR_SIZE, 0);
        m_fileStream.write(zeros.data(), zeros.size());
        
        return static_cast<int>(newStart);
    }

    void AnvilRegionWriter::MarkSectorsFree(int startSector, int count) {
        if (startSector < 2) return; // Don't free header sectors

        for (int i = 0; i < count; ++i) {
            if (startSector + i < static_cast<int>(m_freeSectors.size())) {
                m_freeSectors[startSector + i] = true;
            }
        }
    }

    void AnvilRegionWriter::BuildSectorMap() {
        // Start with file size
        m_fileStream.seekg(0, std::ios::end);
        size_t fileSize = m_fileStream.tellg();
        size_t sectorCount = (fileSize + SECTOR_SIZE - 1) / SECTOR_SIZE;
        
        m_freeSectors.assign(sectorCount, true);
        
        // Mark header sectors as used
        if (m_freeSectors.size() >= 2) {
            m_freeSectors[0] = false;
            m_freeSectors[1] = false;
        }
        
        // Mark sectors used by chunks
        for (const auto& location : m_locations) {
            if (!location.isEmpty()) {
                for (int i = 0; i < location.sectorCount; ++i) {
                    size_t sector = location.sectorOffset + i;
                    if (sector < m_freeSectors.size()) {
                        m_freeSectors[sector] = false;
                    }
                }
            }
        }
    }

    std::vector<uint8_t> AnvilRegionWriter::SerializeChunkToNBT(const Chunk& chunk) {
        std::vector<uint8_t> nbtData;
        
        // Start with root compound
        WriteCompoundStart(nbtData, "");
        
        // DataVersion (required by Minecraft 1.18+)
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Int), "DataVersion");
        WriteInt(nbtData, 2975); // Minecraft 1.18.2 data version
        
        // Level compound (for compatibility)
        WriteCompoundStart(nbtData, "Level");
        
        // Chunk position
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Int), "xPos");
        WriteInt(nbtData, chunk.pos.x);
        
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Int), "zPos");
        WriteInt(nbtData, chunk.pos.z);
        
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Long), "LastUpdate");
        WriteLong(nbtData, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        // Heightmaps
        WriteCompoundStart(nbtData, "Heightmaps");
        
        auto motionBlocking = CalculateMotionBlockingHeightmap(chunk);
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Long_Array), "MOTION_BLOCKING");
        WriteLongArray(nbtData, motionBlocking);
        
        auto worldSurface = CalculateWorldSurfaceHeightmap(chunk);
        WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Long_Array), "WORLD_SURFACE");
        WriteLongArray(nbtData, worldSurface);
        
        WriteCompoundEnd(nbtData); // End Heightmaps
        
        // Structures (empty but required)
        WriteCompoundStart(nbtData, "structures");
        WriteCompoundEnd(nbtData);
        
        // Empty lists (required)
        WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Compound), 0, "block_entities");
        WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Compound), 0, "block_ticks");
        WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Compound), 0, "fluid_ticks");
        
        // PostProcessing (24 empty lists)
        WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_List), 24, "PostProcessing");
        for (int i = 0; i < 24; ++i) {
            WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Byte), 0);
        }
        
        // Sections
        SerializeSections(chunk, nbtData);
        
        WriteCompoundEnd(nbtData); // End Level
        WriteCompoundEnd(nbtData); // End root
        
        return nbtData;
    }

    void AnvilRegionWriter::SerializeSections(const Chunk& chunk, std::vector<uint8_t>& nbtData) {
        // Count non-empty sections
        int sectionCount = 0;
        for (int i = 0; i < Math::SECTIONS_PER_CHUNK; ++i) {
            if (chunk.HasSection(i)) {
                sectionCount++;
            }
        }
        
        WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Compound), sectionCount, "sections");
        
        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            if (!chunk.HasSection(sectionY)) continue;
            
            const ChunkSection* section = chunk.GetSection(sectionY);
            if (!section) continue;
            
            WriteCompoundStart(nbtData); // Section compound (no name in list)
            
            // Section Y coordinate (world section Y, not local)
            int worldSectionY = sectionY + (Config::MinY / Math::SECTION_HEIGHT);
            WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Byte), "Y");
            nbtData.push_back(static_cast<uint8_t>(worldSectionY));
            
            // Simplified block states (single palette entry for now)
            WriteCompoundStart(nbtData, "block_states");
            
            // Palette with air only (simplified)
            WriteListStart(nbtData, static_cast<uint8_t>(NBTType::TAG_Compound), 1, "palette");
            WriteCompoundStart(nbtData);
            WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_String), "Name");
            WriteString(nbtData, "minecraft:air");
            WriteCompoundEnd(nbtData);
            
            WriteCompoundEnd(nbtData); // End block_states
            
            // Basic lighting (all zeros for now)
            WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Byte_Array), "BlockLight");
            std::vector<int8_t> blockLight(2048, 0); // 16x16x16 / 2 = 2048 bytes
            WriteByteArray(nbtData, blockLight);
            
            WriteTagHeader(nbtData, static_cast<uint8_t>(NBTType::TAG_Byte_Array), "SkyLight");
            std::vector<int8_t> skyLight(2048, 15); // Full bright sky light
            WriteByteArray(nbtData, skyLight);
            
            WriteCompoundEnd(nbtData); // End section
        }
    }

    std::vector<int64_t> AnvilRegionWriter::CalculateMotionBlockingHeightmap(const Chunk& chunk) {
        std::vector<int> heights(256); // 16x16 heightmap
        
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                int height = Config::MinY;
                
                // Find highest non-air block
                for (int y = Config::MaxY; y >= Config::MinY; --y) {
                    if (chunk.GetBlock(x, y, z) != BlockID::Air) {
                        height = y + 1; // Heightmap is height of space above block
                        break;
                    }
                }
                
                heights[z * 16 + x] = height - Config::MinY; // Relative to chunk base
            }
        }
        
        return PackHeightmapToLongs(heights);
    }

    std::vector<int64_t> AnvilRegionWriter::CalculateWorldSurfaceHeightmap(const Chunk& chunk) {
        // For simplicity, use same as motion blocking
        return CalculateMotionBlockingHeightmap(chunk);
    }

    std::vector<int64_t> AnvilRegionWriter::PackHeightmapToLongs(const std::vector<int>& heights) {
        // Minecraft uses 9 bits per height value, packed into longs
        // 256 values * 9 bits = 2304 bits = 36 longs
        std::vector<int64_t> longs(36, 0);
        
        const int bitsPerValue = 9;
        const int64_t mask = (1LL << bitsPerValue) - 1;
        
        for (size_t i = 0; i < heights.size() && i < 256; ++i) {
            int64_t value = std::clamp(heights[i], 0, (1 << bitsPerValue) - 1);
            size_t bitIndex = i * bitsPerValue;
            size_t longIndex = bitIndex / 64;
            int bitOffset = bitIndex % 64;
            
            if (longIndex < longs.size()) {
                longs[longIndex] |= (value & mask) << bitOffset;
                
                // Handle overflow to next long
                if (bitOffset + bitsPerValue > 64 && longIndex + 1 < longs.size()) {
                    int remainingBits = (bitOffset + bitsPerValue) - 64;
                    longs[longIndex + 1] |= (value & mask) >> (bitsPerValue - remainingBits);
                }
            }
        }
        
        return longs;
    }

    // === NBT WRITING HELPERS ===

    void AnvilRegionWriter::WriteTagHeader(std::vector<uint8_t>& data, uint8_t tagType, const std::string& name) {
        data.push_back(tagType);
        if (!name.empty()) {
            uint16_t nameLength = static_cast<uint16_t>(name.length());
            data.push_back((nameLength >> 8) & 0xFF); // Big-endian
            data.push_back(nameLength & 0xFF);
            data.insert(data.end(), name.begin(), name.end());
        }
    }

    void AnvilRegionWriter::WriteInt(std::vector<uint8_t>& data, int32_t value) {
        data.push_back((value >> 24) & 0xFF);
        data.push_back((value >> 16) & 0xFF);
        data.push_back((value >> 8) & 0xFF);
        data.push_back(value & 0xFF);
    }

    void AnvilRegionWriter::WriteLong(std::vector<uint8_t>& data, int64_t value) {
        data.push_back((value >> 56) & 0xFF);
        data.push_back((value >> 48) & 0xFF);
        data.push_back((value >> 40) & 0xFF);
        data.push_back((value >> 32) & 0xFF);
        data.push_back((value >> 24) & 0xFF);
        data.push_back((value >> 16) & 0xFF);
        data.push_back((value >> 8) & 0xFF);
        data.push_back(value & 0xFF);
    }

    void AnvilRegionWriter::WriteString(std::vector<uint8_t>& data, const std::string& value) {
        uint16_t length = static_cast<uint16_t>(value.length());
        data.push_back((length >> 8) & 0xFF);
        data.push_back(length & 0xFF);
        data.insert(data.end(), value.begin(), value.end());
    }

    void AnvilRegionWriter::WriteLongArray(std::vector<uint8_t>& data, const std::vector<int64_t>& values) {
        uint32_t length = static_cast<uint32_t>(values.size());
        WriteInt(data, static_cast<int32_t>(length));
        
        for (int64_t value : values) {
            WriteLong(data, value);
        }
    }

    void AnvilRegionWriter::WriteByteArray(std::vector<uint8_t>& data, const std::vector<int8_t>& values) {
        uint32_t length = static_cast<uint32_t>(values.size());
        WriteInt(data, static_cast<int32_t>(length));
        
        for (int8_t value : values) {
            data.push_back(static_cast<uint8_t>(value));
        }
    }

    void AnvilRegionWriter::WriteCompoundStart(std::vector<uint8_t>& data, const std::string& name) {
        WriteTagHeader(data, static_cast<uint8_t>(NBTType::TAG_Compound), name);
    }

    void AnvilRegionWriter::WriteCompoundEnd(std::vector<uint8_t>& data) {
        data.push_back(static_cast<uint8_t>(NBTType::TAG_End));
    }

    void AnvilRegionWriter::WriteListStart(std::vector<uint8_t>& data, uint8_t elementType, int32_t count, const std::string& name) {
        WriteTagHeader(data, static_cast<uint8_t>(NBTType::TAG_List), name);
        data.push_back(elementType);
        WriteInt(data, count);
    }

    // === COMPRESSION ===

    std::vector<uint8_t> AnvilRegionWriter::CompressZlib(const std::vector<uint8_t>& data) {
        if (data.empty()) {
            return {};
        }

        z_stream stream = {};
        if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
            Log::Error("Failed to initialize zlib compression");
            return {};
        }

        stream.avail_in = static_cast<uInt>(data.size());
        stream.next_in = const_cast<Bytef*>(data.data());

        std::vector<uint8_t> compressed;
        compressed.reserve(data.size() / 2); // Estimate compressed size

        int ret;
        do {
            uint8_t outBuffer[4096];
            stream.avail_out = sizeof(outBuffer);
            stream.next_out = outBuffer;

            ret = deflate(&stream, Z_FINISH);

            if (ret == Z_STREAM_ERROR) {
                deflateEnd(&stream);
                Log::Error("Zlib compression stream error");
                return {};
            }

            size_t have = sizeof(outBuffer) - stream.avail_out;
            compressed.insert(compressed.end(), outBuffer, outBuffer + have);

        } while (stream.avail_out == 0);

        deflateEnd(&stream);

        if (ret != Z_STREAM_END) {
            Log::Error("Zlib compression did not complete properly");
            return {};
        }

        Log::Debug("Compressed %zu bytes to %zu bytes (%.1f%% ratio)",
                   data.size(), compressed.size(),
                   (compressed.size() * 100.0f) / data.size());

        return compressed;
    }

    // === UTILITY FUNCTIONS ===

    int AnvilRegionWriter::CoordsToIndex(int localX, int localZ) const {
        return localX + localZ * REGION_SIZE;
    }

    uint32_t AnvilRegionWriter::GetCurrentTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
        return static_cast<uint32_t>(seconds.count());
    }

    bool AnvilRegionWriter::IsValidLocalCoords(int localX, int localZ) const {
        return localX >= 0 && localX < REGION_SIZE && 
               localZ >= 0 && localZ < REGION_SIZE;
    }

    // === FREE FUNCTIONS ===

    bool EnsureRegionFileExists(const std::string& worldPath, Math::ChunkPos chunkPos) {
        int regionX, regionZ, localX, localZ;
        AnvilRegionWriter::ChunkToRegion(chunkPos, regionX, regionZ, localX, localZ);
        
        std::string regionFilePath = AnvilRegionWriter::GenerateRegionFilePath(worldPath, regionX, regionZ);
        
        // Check if region file already exists
        if (std::filesystem::exists(regionFilePath)) {
            return true;
        }
        
        // Create region file
        AnvilRegionWriter writer(regionFilePath);
        if (!writer.Initialize()) {
            Log::Error("Failed to create region file: %s", regionFilePath.c_str());
            return false;
        }
        
        writer.Finalize();
        Log::Info("Created new region file: %s", regionFilePath.c_str());
        return true;
    }

    bool SaveChunkToAnvilFormat(const std::string& worldPath, const Chunk& chunk) {
        if (chunk.IsEmpty()) {
            Log::Debug("Skipping save of empty chunk (%d, %d)", chunk.pos.x, chunk.pos.z);
            return true;
        }
        
        // Ensure world structure exists
        if (!CreateMinecraftWorldStructure(worldPath)) {
            Log::Error("Failed to create world structure");
            return false;
        }
        
        // Calculate region coordinates
        int regionX, regionZ, localX, localZ;
        AnvilRegionWriter::ChunkToRegion(chunk.pos, regionX, regionZ, localX, localZ);
        
        std::string regionFilePath = AnvilRegionWriter::GenerateRegionFilePath(worldPath, regionX, regionZ);
        
        // Open region writer
        AnvilRegionWriter writer(regionFilePath);
        if (!writer.Initialize()) {
            Log::Error("Failed to initialize region writer for %s", regionFilePath.c_str());
            return false;
        }
        
        // Write chunk
        if (!writer.WriteChunk(localX, localZ, chunk)) {
            Log::Error("Failed to write chunk (%d, %d) to region file", chunk.pos.x, chunk.pos.z);
            return false;
        }
        
        // Finalize
        if (!writer.Finalize()) {
            Log::Error("Failed to finalize region file");
            return false;
        }
        
        Log::Info("Successfully saved chunk (%d, %d) to Anvil format", chunk.pos.x, chunk.pos.z);
        return true;
    }

    bool CreateMinecraftWorldStructure(const std::string& worldPath) {
        try {
            // Create world directory
            std::filesystem::create_directories(worldPath);
            
            // Create region directory
            std::filesystem::path regionDir = std::filesystem::path(worldPath) / "region";
            std::filesystem::create_directories(regionDir);
            
            // Create level.dat (minimal)
            std::filesystem::path levelDat = std::filesystem::path(worldPath) / "level.dat";
            if (!std::filesystem::exists(levelDat)) {
                // Create minimal level.dat
                std::ofstream levelFile(levelDat, std::ios::binary);
                if (levelFile.is_open()) {
                    // Write minimal NBT structure for level.dat
                    // This is a simplified version - real level.dat is more complex
                    std::vector<uint8_t> levelData = {
                        0x0A, 0x00, 0x00, // Compound tag with empty name
                        0x08, 0x00, 0x04, 'D', 'a', 't', 'a', // String tag "Data"
                        0x00, 0x05, 'W', 'o', 'r', 'l', 'd', // Value "World"
                        0x00 // End compound
                    };
                    levelFile.write(reinterpret_cast<const char*>(levelData.data()), levelData.size());
                    levelFile.close();
                }
            }
            
            Log::Info("Created Minecraft world structure at: %s", worldPath.c_str());
            return true;
            
        } catch (const std::exception& e) {
            Log::Error("Failed to create world structure: %s", e.what());
            return false;
        }
    }

} // namespace Game