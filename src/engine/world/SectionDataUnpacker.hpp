// File: src/engine/world/SectionDataUnpacker.hpp
#pragma once

#include "../block/Blocks.hpp"
#include "../../game/WorldMath.hpp"
#include "../world/NBTParser.hpp"
#include "../../core/Log.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace Game {

    // Forward declarations
    class Chunk;
    class ChunkSection;

    // Represents a Minecraft block state (block type + properties)
    struct BlockState {
        std::string name;                                    // e.g. "minecraft:grass_block"
        std::unordered_map<std::string, std::string> properties; // e.g. {"snowy": "false"}
        BlockID resolvedId;                                  // Our internal block ID

        BlockState() : resolvedId(BlockID::Air) {}
        BlockState(const std::string& blockName, BlockID id = BlockID::Air)
            : name(blockName), resolvedId(id) {}

        // Generate a string key for this block state (for caching/lookup)
        std::string GetStateKey() const {
            std::string key = name;
            if (!properties.empty()) {
                key += "{";
                bool first = true;
                for (const auto& [prop, value] : properties) {
                    if (!first) key += ",";
                    key += prop + ":" + value;
                    first = false;
                }
                key += "}";
            }
            return key;
        }
    };

    // Registry for converting Minecraft block names to our internal BlockID enum
    class BlockStateRegistry {
    public:
        static void Initialize();
        static BlockID ResolveBlockState(const BlockState& state);
        static BlockState CreateBlockState(const std::string& name,
                                         const std::unordered_map<std::string, std::string>& props = {});

    private:
        static std::unordered_map<std::string, BlockID> s_nameToBlockId;
        static std::unordered_map<std::string, BlockID> s_stateToBlockId; // For specific states
        static bool s_initialized;

        static std::string NormalizeName(const std::string& name);
    };

    // Section data unpacker - converts Minecraft section NBT to our chunk format
    class SectionDataUnpacker {
    public:
        // Main entry point: unpack all sections from a chunk's NBT data
        static bool UnpackChunkSections(const World::NBTTagPtr& chunkNBT, Chunk& chunk);

        // Unpack a single section from NBT
        static bool UnpackSection(const World::NBTTagPtr& sectionNBT, Chunk& chunk, int sectionY);

        // Parse palette from section NBT
        static std::vector<BlockState> ParsePalette(const World::NBTTagPtr& paletteList);

        // Unpack packed block data using palette
        static bool UnpackBlockData(const std::vector<uint64_t>& packedData,
                                   const std::vector<BlockState>& palette,
                                   Chunk& chunk, int sectionY);

        // Calculate bits per block for a palette
        static int CalculateBitsPerBlock(size_t paletteSize);

        // Extract a value from packed long array
        static uint64_t ExtractPackedValue(const std::vector<uint64_t>& data,
                                         int bitIndex, int bitsPerBlock);

        // Convert block index to local coordinates within section
        static void IndexToCoords(int blockIndex, int& x, int& y, int& z);

        // Convert section-relative coordinates to world coordinates
        static void SectionToWorldCoords(int sectionX, int sectionY, int sectionZ,
                                       int sectionYLevel, int chunkX, int chunkZ,
                                       int& worldX, int& worldY, int& worldZ);

        // Debug: print section statistics
        static void PrintSectionStats(const std::vector<BlockState>& palette,
                                     int sectionY, int blockCount);

    private:
        SectionDataUnpacker() = delete; // Static utility class
    };

} // namespace Game