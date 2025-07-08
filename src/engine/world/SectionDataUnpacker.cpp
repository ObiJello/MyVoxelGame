// File: src/engine/world/SectionDataUnpacker.cpp
#include "SectionDataUnpacker.hpp"
#include "Chunk.hpp"
#include "ChunkSection.hpp"
#include "../block/BlockRegistry.hpp"
#include "../../core/Config.hpp"
#include <algorithm>
#include <unordered_set>

namespace Game {

    // BlockStateRegistry implementation
    std::unordered_map<std::string, BlockID> BlockStateRegistry::s_nameToBlockId;
    std::unordered_map<std::string, BlockID> BlockStateRegistry::s_stateToBlockId;
    bool BlockStateRegistry::s_initialized = false;

    void BlockStateRegistry::Initialize() {
        if (s_initialized) return;

        Log::Info("Initializing BlockStateRegistry...");

        // Map Minecraft block names to our internal BlockID enum
        // NOTE: These mappings should match your BlockRegistry::Init() blocks
        s_nameToBlockId["minecraft:air"] = BlockID::Air;
        s_nameToBlockId["minecraft:stone"] = BlockID::Stone;
        s_nameToBlockId["minecraft:dirt"] = BlockID::Dirt;
        s_nameToBlockId["minecraft:grass_block"] = BlockID::Grass;
        s_nameToBlockId["minecraft:sand"] = BlockID::Sand;
        s_nameToBlockId["minecraft:sandstone"] = BlockID::Sandstone;
        s_nameToBlockId["minecraft:oak_log"] = BlockID::OakLog;
        s_nameToBlockId["minecraft:snow_block"] = BlockID::Snow;
        s_nameToBlockId["minecraft:ice"] = BlockID::Ice;
        s_nameToBlockId["minecraft:glass"] = BlockID::Glass;
        s_nameToBlockId["minecraft:bedrock"] = BlockID::Bedrock;
        s_nameToBlockId["minecraft:water"] = BlockID::Water;
        s_nameToBlockId["minecraft:oak_leaves"] = BlockID::Leaves;
        s_nameToBlockId["minecraft:cherry_log"] = BlockID::CherryLog;
        s_nameToBlockId["minecraft:birch_log"] = BlockID::BirchLog;
        s_nameToBlockId["minecraft:acacia_log"] = BlockID::AcaciaLog;
        s_nameToBlockId["minecraft:cherry_leaves"] = BlockID::CherryLeaves;
        s_nameToBlockId["minecraft:coal_ore"] = BlockID::CoalOre;
        s_nameToBlockId["minecraft:redstone_ore"] = BlockID::RedstoneOre;
        s_nameToBlockId["minecraft:lapis_ore"] = BlockID::LapisOre;
        s_nameToBlockId["minecraft:iron_ore"] = BlockID::IronOre;
        s_nameToBlockId["minecraft:gold_ore"] = BlockID::GoldOre;
        s_nameToBlockId["minecraft:emerald_ore"] = BlockID::EmeraldOre;
        s_nameToBlockId["minecraft:diamond_ore"] = BlockID::DiamondOre;
        s_nameToBlockId["minecraft:gravel"] = BlockID::Gravel;
        s_nameToBlockId["minecraft:mycelium"] = BlockID::Mycelium;
        s_nameToBlockId["minecraft:deepslate"] = BlockID::Deepslate;

        // Add common block variants
        s_nameToBlockId["minecraft:grass"] = BlockID::Grass; // Legacy name
        s_nameToBlockId["minecraft:snow"] = BlockID::Snow;   // Alternative name

        // Add specific block states that need special handling
        s_stateToBlockId["minecraft:grass_block{snowy:true}"] = BlockID::SnowGrass;

        // Log statistics
        Log::Info("BlockStateRegistry initialized with %zu base blocks and %zu specific states",
                 s_nameToBlockId.size(), s_stateToBlockId.size());

        s_initialized = true;
    }

    BlockID BlockStateRegistry::ResolveBlockState(const BlockState& state) {
        if (!s_initialized) Initialize();

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

        // Unknown block - log warning and return air
        static std::unordered_set<std::string> loggedUnknown;
        if (loggedUnknown.find(normalizedName) == loggedUnknown.end()) {
            Log::Warning("Unknown block state: %s", stateKey.c_str());
            loggedUnknown.insert(normalizedName);
        }

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

        // Get block_states compound
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

        // Parse palette
        auto paletteTag = blockStatesCompound->GetTag("palette");
        if (!paletteTag) {
            Log::Error("No palette found in section Y=%d", sectionY);
            return false;
        }

        std::vector<BlockState> palette = ParsePalette(paletteTag);
        if (palette.empty()) {
            Log::Error("Empty palette in section Y=%d", sectionY);
            return false;
        }

        Log::Debug("Section Y=%d has palette with %zu entries", sectionY, palette.size());

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
            for (int x = 0; x < ChunkSection::SIZE; ++x) {
                for (int y = 0; y < ChunkSection::SIZE; ++y) {
                    for (int z = 0; z < ChunkSection::SIZE; ++z) {
                        chunk.sections[sectionIndex]->Set(x, y, z, singleBlock);
                    }
                }
            }

            PrintSectionStats(palette, sectionY, ChunkSection::SIZE * ChunkSection::SIZE * ChunkSection::SIZE);
            return true;
        }

        // Get packed data for multi-block sections
        auto dataTag = blockStatesCompound->GetTag("data");
        if (!dataTag) {
            Log::Error("No data array found in section Y=%d", sectionY);
            return false;
        }

        auto dataArray = std::dynamic_pointer_cast<World::NBTTagLongArray>(dataTag);
        if (!dataArray) {
            Log::Error("Data is not a long array in section Y=%d", sectionY);
            return false;
        }

        // Convert to uint64_t vector
        std::vector<uint64_t> packedData;
        packedData.reserve(dataArray->value.size());
        for (int64_t val : dataArray->value) {
            packedData.push_back(static_cast<uint64_t>(val));
        }

        Log::Debug("Section Y=%d has %zu longs of packed data", sectionY, packedData.size());

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

        // Validate data size
        int expectedLongs = (4096 * bitsPerBlock + 63) / 64; // Ceiling division
        if (static_cast<int>(packedData.size()) < expectedLongs) {
            Log::Error("Not enough packed data: have %zu longs, need %d for section Y=%d",
                      packedData.size(), expectedLongs, sectionY);
            return false;
        }

        uint64_t mask = (1ULL << bitsPerBlock) - 1;
        int blocksProcessed = 0;
        std::unordered_map<BlockID, int> blockCounts; // For statistics

        // Loop over all 4096 blocks in the section
        for (int blockIndex = 0; blockIndex < 4096; ++blockIndex) {
            // Extract palette index from packed data
            uint64_t paletteIndex = ExtractPackedValue(packedData, blockIndex * bitsPerBlock, bitsPerBlock);

            // Validate palette index
            if (paletteIndex >= palette.size()) {
                Log::Error("Invalid palette index %llu (max %zu) at block %d in section Y=%d",
                          paletteIndex, palette.size() - 1, blockIndex, sectionY);
                continue;
            }

            // Get block ID from palette
            BlockID blockId = palette[paletteIndex].resolvedId;

            // Convert block index to local coordinates
            int x, y, z;
            IndexToCoords(blockIndex, x, y, z);

            // Set block in section
            chunk.sections[sectionIndex]->Set(x, y, z, blockId);

            // Update statistics
            blockCounts[blockId]++;
            blocksProcessed++;
        }

        PrintSectionStats(palette, sectionY, blocksProcessed);

        // Log block distribution for debugging
        if (blockCounts.size() <= 10) { // Only log if not too many different blocks
            Log::Debug("Section Y=%d block distribution:", sectionY);
            for (const auto& [blockId, count] : blockCounts) {
                const Block& block = BlockRegistry::Get(blockId);
                Log::Debug("  %s: %d blocks", block.name.c_str(), count);
            }
        }

        return true;
    }

    int SectionDataUnpacker::CalculateBitsPerBlock(size_t paletteSize) {
        if (paletteSize <= 1) return 0; // Single block
        return std::max(4, static_cast<int>(std::ceil(std::log2(paletteSize))));
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