// File: src/server/world/storage/SectionDataUnpacker.cpp
#include "SectionDataUnpacker.hpp"
#include "common/world/chunk/ChunkSection.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/core/Config.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include <iomanip>
#include <ctime>

namespace Game {

    // UnimplementedBlockTracker implementation
    UnimplementedBlockTracker& UnimplementedBlockTracker::GetInstance() {
        static UnimplementedBlockTracker instance;
        return instance;
    }
    
    void UnimplementedBlockTracker::TrackUnimplementedBlock(const std::string& blockName) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blockCounts[blockName]++;
        // Just track, don't save - saving happens only on shutdown
    }
    
    size_t UnimplementedBlockTracker::GetTotalConversions() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t total = 0;
        for (const auto& [name, count] : m_blockCounts) {
            total += count;
        }
        return total;
    }
    
    std::string UnimplementedBlockTracker::GetOutputPath() const {
        // Use the game directory for storing the report
        return Platform::g_gameDirectory.GetGameDirectory() + "/unimplemented_blocks.txt";
    }
    
    void UnimplementedBlockTracker::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_blockCounts.clear();
    }
    
    void UnimplementedBlockTracker::SaveToFile() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (m_blockCounts.empty()) {
            return; // Nothing to save
        }
        
        std::string outputPath = GetOutputPath();
        std::ofstream file(outputPath);
        if (!file.is_open()) {
            Log::Warning("Failed to open unimplemented blocks report file: %s", outputPath.c_str());
            return;
        }
        
        // Get current time
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        // Write header
        file << "=== Unimplemented Blocks Report ===" << std::endl;
        file << "Generated: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << std::endl;
        file << "Total unique blocks: " << m_blockCounts.size() << std::endl;
        
        // Calculate total directly since we already hold the lock
        size_t total = 0;
        for (const auto& [name, count] : m_blockCounts) {
            total += count;
        }
        file << "Total conversions: " << total << std::endl;
        file << std::endl;
        
        // Sort blocks by count (descending)
        std::vector<std::pair<std::string, size_t>> sortedBlocks(m_blockCounts.begin(), m_blockCounts.end());
        std::sort(sortedBlocks.begin(), sortedBlocks.end(), 
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        
        // Write column headers
        file << std::left << std::setw(10) << "Count" << "Block Name" << std::endl;
        file << std::left << std::setw(10) << "-----" << "----------" << std::endl;
        
        // Write sorted block list
        for (const auto& [blockName, count] : sortedBlocks) {
            file << std::left << std::setw(10) << count << blockName << std::endl;
        }
        
        file.close();
        Log::Info("Saved unimplemented blocks report to: %s (%zu unique blocks)", 
                  outputPath.c_str(), m_blockCounts.size());
    }

    // BlockStateRegistry implementation
    std::unordered_map<std::string, BlockID> BlockStateRegistry::s_nameToBlockId;
    std::unordered_map<std::string, BlockID> BlockStateRegistry::s_stateToBlockId;
    std::once_flag BlockStateRegistry::s_initFlag;

    void BlockStateRegistry::Initialize() {
        std::call_once(s_initFlag, []() {
            Log::Info("Initializing BlockStateRegistry...");

            // Air must be manual (not in .inc)
            s_nameToBlockId["minecraft:air"] = BlockID::Air;

            // All blocks from BlockDefs.inc (single source of truth)
            #define BLOCK_DEF(e, m, d, o) s_nameToBlockId["minecraft:" m] = BlockID::e;
            #include "common/world/block/BlockDefs.inc"
            #undef BLOCK_DEF

            // Manual aliases
            s_nameToBlockId["minecraft:grass"] = BlockID::Grass;
            s_nameToBlockId["minecraft:cave_air"] = BlockID::Air;
            s_nameToBlockId["minecraft:void_air"] = BlockID::Air;
            s_nameToBlockId["minecraft:snow"] = BlockID::Snow;

            // Specific block state overrides
            s_stateToBlockId["minecraft:grass_block{snowy:true}"] = BlockID::SnowGrass;

            // Log statistics
            Log::Info("BlockStateRegistry initialized with %zu base blocks and %zu specific states",
                     s_nameToBlockId.size(), s_stateToBlockId.size());
        });
    }

    BlockID BlockStateRegistry::ResolveBlockState(const BlockState& state) {
        Initialize();

        // First try exact state match (for blocks with important properties)
        std::string stateKey = state.GetStateKey();
        auto stateIt = s_stateToBlockId.find(stateKey);
        if (stateIt != s_stateToBlockId.end()) {
            return stateIt->second;
        }

        // Then try base block name
        std::string normalizedName = NormalizeName(state.name);
        auto nameIt = s_nameToBlockId.find(normalizedName);
        if (nameIt != s_nameToBlockId.end()) {
            return nameIt->second;
        }

        // Try without namespace prefix
        if (normalizedName.find("minecraft:") == 0) {
            std::string withoutPrefix = normalizedName.substr(10); // Remove "minecraft:"
            auto prefixIt = s_nameToBlockId.find(withoutPrefix);
            if (prefixIt != s_nameToBlockId.end()) {
                return prefixIt->second;
            }
        }

        // Unknown block - track it and return air
        static std::unordered_set<std::string> loggedUnknown;
        static std::mutex loggedUnknownMutex;
        {
            std::lock_guard<std::mutex> lock(loggedUnknownMutex);
            if (loggedUnknown.find(normalizedName) == loggedUnknown.end()) {
                Log::Warning("Unknown block state: %s", stateKey.c_str());
                loggedUnknown.insert(normalizedName);
            }
        }
        
        // Track this unimplemented block
        UnimplementedBlockTracker::GetInstance().TrackUnimplementedBlock(normalizedName);

        return BlockID::Air; // Default fallback
    }

    BlockState BlockStateRegistry::CreateBlockState(const std::string& name,
                                                   const std::unordered_map<std::string, std::string>& props) {
        BlockState state;
        state.name = NormalizeName(name);
        state.properties = props;
        state.resolvedId = ResolveBlockState(state);
        return state;
    }

    std::string BlockStateRegistry::NormalizeName(const std::string& name) {
        // Ensure name has minecraft: prefix
        if (name.find(':') == std::string::npos) {
            return "minecraft:" + name;
        }
        return name;
    }

    // SectionDataUnpacker implementation
    bool SectionDataUnpacker::UnpackChunkSections(const World::NBTTagPtr& chunkNBT, Chunk& chunk) {
        if (!chunkNBT) {
            Log::Error("Null chunk NBT passed to UnpackChunkSections");
            return false;
        }

        auto rootCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(chunkNBT);
        if (!rootCompound) {
            Log::Error("Chunk NBT root is not a compound tag");
            return false;
        }

        // Initialize block state registry
        BlockStateRegistry::Initialize();

        // Get the Level compound (pre-1.18) or look for sections at root level (1.18+)
        World::NBTTagPtr sectionsTag = nullptr;
        auto levelTag = rootCompound->GetTag("Level");

        if (levelTag) {
            // Pre-1.18 format: sections are in Level compound
            auto levelCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(levelTag);
            if (levelCompound) {
                sectionsTag = levelCompound->GetTag("sections");
            }
        } else {
            // 1.18+ format: sections might be at root level
            sectionsTag = rootCompound->GetTag("sections");
        }

        if (!sectionsTag) {
            Log::Warning("No sections found in chunk NBT");
            return false;
        }

        auto sectionsList = std::dynamic_pointer_cast<World::NBTTagList>(sectionsTag);
        if (!sectionsList) {
            Log::Error("Sections tag is not a list");
            return false;
        }

        Log::Debug("Found %zu sections in chunk", sectionsList->value.size());

        int processedSections = 0;
        int successfulSections = 0;

        // Process each section
        for (const auto& sectionTagPtr : sectionsList->value) {
            auto sectionCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(sectionTagPtr);
            if (!sectionCompound) {
                Log::Warning("Section is not a compound tag, skipping");
                continue;
            }

            processedSections++;

            // Get section Y level
            int8_t sectionY = sectionCompound->GetValue<int8_t>("Y", 0);

            Log::Debug("Processing section Y=%d", static_cast<int>(sectionY));

            // Unpack this section
            if (UnpackSection(sectionCompound, chunk, sectionY)) {
                successfulSections++;
            } else {
                Log::Warning("Failed to unpack section Y=%d", static_cast<int>(sectionY));
            }
        }

        Log::Info("Unpacked %d/%d sections successfully", successfulSections, processedSections);
        return successfulSections > 0;
    }

    bool SectionDataUnpacker::UnpackSection(const World::NBTTagPtr& sectionNBT,
                                           Chunk& chunk, int sectionY) {
        auto sectionCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(sectionNBT);
        if (!sectionCompound) {
            return false;
        }

        // **CRITICAL FIX**: Get block_states compound first, then get palette from within it
        auto blockStatesTag = sectionCompound->GetTag("block_states");
        if (!blockStatesTag) {
            Log::Debug("Section Y=%d has no block_states, filling with air", sectionY);
            return true; // Empty section is valid, just fill with air
        }

        auto blockStatesCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(blockStatesTag);
        if (!blockStatesCompound) {
            Log::Error("block_states is not a compound in section Y=%d", sectionY);
            return false;
        }

        // **FIXED**: Parse palette from INSIDE block_states, not from section root
        auto paletteTag = blockStatesCompound->GetTag("palette");
        if (!paletteTag) {
            Log::Error("No palette found in block_states for section Y=%d", sectionY);
            return false;
        }

        std::vector<BlockState> palette = ParsePalette(paletteTag);
        if (palette.empty()) {
            Log::Error("Empty palette in section Y=%d", sectionY);
            return false;
        }

        Log::Debug("Section Y=%d has BLOCK palette with %zu entries", sectionY, palette.size());

        // If palette has only one entry, fill entire section with that block
        if (palette.size() == 1) {
            BlockID singleBlock = palette[0].resolvedId;

            // Convert section Y to our section index
            int sectionIndex = sectionY - (Config::MinY / Math::SECTION_HEIGHT);
            if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) {
                Log::Warning("Section Y=%d maps to invalid section index %d", sectionY, sectionIndex);
                return false;
            }

            // Create section if needed and fill with single block
            if (!chunk.sections[sectionIndex]) {
                chunk.sections[sectionIndex] = std::make_unique<ChunkSection>();
            }

            // Fill entire section with the single block type
            for (int y = 0; y < ChunkSection::SIZE; ++y) {
                for (int z = 0; z < ChunkSection::SIZE; ++z) {
                    for (int x = 0; x < ChunkSection::SIZE; ++x) {
                        chunk.sections[sectionIndex]->Set(x, y, z, singleBlock);
                    }
                }
            }

            PrintSectionStats(palette, sectionY, ChunkSection::SIZE * ChunkSection::SIZE * ChunkSection::SIZE);
            return true;
        }

        // **FIXED**: Get packed data from INSIDE block_states, not from section root
        auto dataTag = blockStatesCompound->GetTag("data");
        if (!dataTag) {
            Log::Error("No data array found in block_states for section Y=%d", sectionY);
            return false;
        }

        auto dataArray = std::dynamic_pointer_cast<World::NBTTagLongArray>(dataTag);
        if (!dataArray) {
            Log::Error("Data is not a long array in block_states for section Y=%d", sectionY);
            return false;
        }

        // Convert to uint64_t vector
        std::vector<uint64_t> packedData;
        packedData.reserve(dataArray->value.size());
        for (int64_t val : dataArray->value) {
            packedData.push_back(static_cast<uint64_t>(val));
        }

        Log::Debug("Section Y=%d has %zu longs of packed BLOCK data", sectionY, packedData.size());

        // Unpack the block data
        return UnpackBlockData(packedData, palette, chunk, sectionY);
    }

    std::vector<BlockState> SectionDataUnpacker::ParsePalette(const World::NBTTagPtr& paletteList) {
        std::vector<BlockState> palette;

        auto paletteListTag = std::dynamic_pointer_cast<World::NBTTagList>(paletteList);
        if (!paletteListTag) {
            Log::Error("Palette is not a list tag");
            return palette;
        }

        palette.reserve(paletteListTag->value.size());

        for (const auto& entryPtr : paletteListTag->value) {
            auto entryCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(entryPtr);
            if (!entryCompound) {
                Log::Warning("Palette entry is not a compound, skipping");
                continue;
            }

            // Get block name
            std::string blockName = entryCompound->GetValue<std::string>("Name", "minecraft:air");

            // Get properties (optional)
            std::unordered_map<std::string, std::string> properties;
            auto propertiesTag = entryCompound->GetTag("Properties");
            if (propertiesTag) {
                auto propertiesCompound = std::dynamic_pointer_cast<World::NBTTagCompound>(propertiesTag);
                if (propertiesCompound) {
                    for (const auto& [propName, propTag] : propertiesCompound->value) {
                        auto stringTag = std::dynamic_pointer_cast<World::NBTTagString>(propTag);
                        if (stringTag) {
                            properties[propName] = stringTag->value;
                        }
                    }
                }
            }

            // Create block state
            BlockState state = BlockStateRegistry::CreateBlockState(blockName, properties);
            palette.push_back(state);

            Log::Debug("Palette entry: %s -> BlockID::%d",
                      state.GetStateKey().c_str(), static_cast<int>(state.resolvedId));
        }

        return palette;
    }

    bool SectionDataUnpacker::UnpackBlockData(const std::vector<uint64_t>& packedData,
                                             const std::vector<BlockState>& palette,
                                             Chunk& chunk, int sectionY) {
        if (palette.empty()) {
            Log::Error("Empty palette for section Y=%d", sectionY);
            return false;
        }

        // Convert section Y to our section index
        int sectionIndex = sectionY - (Config::MinY / Math::SECTION_HEIGHT);
        if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) {
            Log::Warning("Section Y=%d maps to invalid section index %d", sectionY, sectionIndex);
            return false;
        }

        // Create section if needed
        if (!chunk.sections[sectionIndex]) {
            chunk.sections[sectionIndex] = std::make_unique<ChunkSection>();
        }

        // Calculate bits per block
        int bitsPerBlock = CalculateBitsPerBlock(palette.size());
        Log::Debug("Section Y=%d using %d bits per block for palette size %zu",
                  sectionY, bitsPerBlock, palette.size());

        // **CRITICAL FIX**: Use new 1.16+ unpacking that respects long boundaries
        return UnpackBlockDataPost116(packedData, palette, *chunk.sections[sectionIndex],
                                     bitsPerBlock, sectionY);
    }

    // **NEW METHOD**: Unpacking for Minecraft 1.16+ format (no crossing long boundaries)
    bool SectionDataUnpacker::UnpackBlockDataPost116(const std::vector<uint64_t>& packedData,
                                                     const std::vector<BlockState>& palette,
                                                     ChunkSection& section,
                                                     int bitsPerBlock, int sectionY) {
        // Calculate how many indices fit in one 64-bit long
        int indicesPerLong = 64 / bitsPerBlock;

        // Calculate expected number of longs needed
        int expectedLongs = (4096 + indicesPerLong - 1) / indicesPerLong; // Ceiling division

        Log::Debug("Section Y=%d: bitsPerBlock=%d, indicesPerLong=%d, expectedLongs=%d, actualLongs=%zu",
                  sectionY, bitsPerBlock, indicesPerLong, expectedLongs, packedData.size());

        if (static_cast<int>(packedData.size()) < expectedLongs) {
            Log::Error("Not enough packed data: have %zu longs, need %d for section Y=%d",
                      packedData.size(), expectedLongs, sectionY);
            return false;
        }

        uint64_t mask = (1ULL << bitsPerBlock) - 1;
        int blocksProcessed = 0;
        std::unordered_map<BlockID, int> blockCounts; // For statistics

        // Process each long
        for (int longIndex = 0; longIndex < expectedLongs && longIndex < static_cast<int>(packedData.size()); ++longIndex) {
            uint64_t currentLong = packedData[longIndex];

            // Extract indices from this long (no cross-boundary spanning)
            for (int indexInLong = 0; indexInLong < indicesPerLong && blocksProcessed < 4096; ++indexInLong) {
                // Extract the palette index from the current position
                int bitPosition = indexInLong * bitsPerBlock;
                uint64_t paletteIndex = (currentLong >> bitPosition) & mask;

                // Validate palette index
                if (paletteIndex >= palette.size()) {
                    Log::Error("Invalid palette index %llu (max %zu) at block %d in section Y=%d",
                              paletteIndex, palette.size() - 1, blocksProcessed, sectionY);
                    continue;
                }

                // Get block ID from palette
                BlockID blockId = palette[paletteIndex].resolvedId;

                // Convert block index to local coordinates
                int x, y, z;
                IndexToCoords(blocksProcessed, x, y, z);

                // Set block in section
                section.Set(x, y, z, blockId);

                // Update statistics
                blockCounts[blockId]++;
                blocksProcessed++;
            }
        }

        Log::Debug("Section Y=%d unpacked %d blocks successfully", sectionY, blocksProcessed);

        // Log block distribution for debugging
        if (blockCounts.size() <= 10) { // Only log if not too many different blocks
            Log::Debug("Section Y=%d block distribution:", sectionY);
            for (const auto& [blockId, count] : blockCounts) {
                const Block& block = BlockRegistry::Get(blockId);
                Log::Debug("  %s: %d blocks", block.name.c_str(), count);
            }
        }

        return blocksProcessed > 0;
    }

    int SectionDataUnpacker::CalculateBitsPerBlock(size_t paletteSize) {
        if (paletteSize <= 1) return 0; // Single block

        // Calculate the minimum bits needed to represent all palette indices
        int bitsNeeded = static_cast<int>(std::ceil(std::log2(paletteSize)));

        // Minecraft uses a minimum of 4 bits per block
        return std::max(4, bitsNeeded);
    }

    uint64_t SectionDataUnpacker::ExtractPackedValue(const std::vector<uint64_t>& data,
                                                    int bitIndex, int bitsPerBlock) {
        int longIndex = bitIndex >> 6;        // Divide by 64
        int startBit = bitIndex & 63;         // Modulo 64

        if (longIndex >= static_cast<int>(data.size())) {
            return 0; // Out of bounds
        }

        uint64_t value = data[longIndex] >> startBit;
        int availableBits = 64 - startBit;

        // If value spans two longs, get remaining bits from next long
        if (availableBits < bitsPerBlock && longIndex + 1 < static_cast<int>(data.size())) {
            value |= data[longIndex + 1] << availableBits;
        }

        // Apply mask to get only the bits we need
        uint64_t mask = (1ULL << bitsPerBlock) - 1;
        return value & mask;
    }

    void SectionDataUnpacker::IndexToCoords(int blockIndex, int& x, int& y, int& z) {
        // Minecraft uses YZX ordering for block indices
        x = blockIndex & 15;           // blockIndex % 16
        z = (blockIndex >> 4) & 15;    // (blockIndex / 16) % 16
        y = blockIndex >> 8;           // blockIndex / 256
    }

    void SectionDataUnpacker::SectionToWorldCoords(int sectionX, int sectionY, int sectionZ,
                                                  int sectionYLevel, int chunkX, int chunkZ,
                                                  int& worldX, int& worldY, int& worldZ) {
        worldX = chunkX * Math::CHUNK_SIZE_X + sectionX;
        worldY = sectionYLevel * Math::SECTION_HEIGHT + sectionY;
        worldZ = chunkZ * Math::CHUNK_SIZE_Z + sectionZ;
    }

    void SectionDataUnpacker::PrintSectionStats(const std::vector<BlockState>& palette,
                                               int sectionY, int blockCount) {
        Log::Debug("Section Y=%d unpacked: %d blocks, palette size: %zu, bits per block: %d",
                  sectionY, blockCount, palette.size(), CalculateBitsPerBlock(palette.size()));

        // Log first few palette entries for debugging
        if (palette.size() <= 5) {
            Log::Debug("  Full palette:");
            for (size_t i = 0; i < palette.size(); ++i) {
                Log::Debug("    [%zu] %s", i, palette[i].GetStateKey().c_str());
            }
        } else {
            Log::Debug("  Sample palette entries:");
            for (size_t i = 0; i < 3 && i < palette.size(); ++i) {
                Log::Debug("    [%zu] %s", i, palette[i].GetStateKey().c_str());
            }
            Log::Debug("    ... and %zu more", palette.size() - 3);
        }
    }

} // namespace Game