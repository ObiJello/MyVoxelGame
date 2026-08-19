// File: src/server/world/storage/SectionDataUnpacker.hpp
#pragma once

#include "common/world/block/Blocks.hpp"
#include "common/world/math/WorldMath.hpp"
#include "NBTParser.hpp"
#include "common/core/Log.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <chrono>

namespace Game {

    // Forward declarations
    class Chunk;
    class ChunkSection;

    // Represents a Minecraft block state (block type + properties)
    struct NbtBlockState {
        std::string name;                                    // e.g. "minecraft:grass_block"
        std::unordered_map<std::string, std::string> properties; // e.g. {"snowy": "false"}
        BlockID resolvedId;                                  // Our internal block ID
        // Index into resolvedId's own state list (MC NbtBlockState.getId()),
        // derived from `properties`. 0 = the block's default state, which is
        // also what an unmodelled property set resolves to.
        BlockStateIndex resolvedState = 0;

        NbtBlockState() : resolvedId(BlockID::Air) {}
        NbtBlockState(const std::string& blockName, BlockID id = BlockID::Air)
            : name(blockName), resolvedId(id) {}

        // Canonical string key for this block state, matching MC's
        // `NbtBlockState.toString()` shape: `minecraft:<name>{p1:v1,p2:v2}` with
        // properties sorted by name.
        //
        // The sort is load-bearing, not cosmetic: `properties` is an
        // unordered_map, so iterating it raw emits property order that varies
        // with hashing and bucket count. Any key built that way is unusable as
        // a lookup key or a log line.
        std::string GetStateKey() const {
            std::string key = name;
            if (!properties.empty()) {
                std::vector<const std::pair<const std::string, std::string>*> sorted;
                sorted.reserve(properties.size());
                for (const auto& kv : properties) sorted.push_back(&kv);
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto* a, const auto* b) { return a->first < b->first; });

                key += "{";
                bool first = true;
                for (const auto* kv : sorted) {
                    if (!first) key += ",";
                    key += kv->first + ":" + kv->second;
                    first = false;
                }
                key += "}";
            }
            return key;
        }

        // `name{prop:value}` — the single-property form the override table is
        // keyed on. A block with several properties can't be looked up by its
        // full key against those entries, so resolution tries each property in
        // turn (see BlockStateRegistry::ResolveBlockState).
        std::string GetSinglePropertyKey(const std::string& prop, const std::string& value) const {
            return name + "{" + prop + ":" + value + "}";
        }
    };

    // Tracker for unimplemented blocks to help prioritize what to implement next
    class UnimplementedBlockTracker {
    public:
        static UnimplementedBlockTracker& GetInstance();
        
        // Track an unimplemented block
        void TrackUnimplementedBlock(const std::string& blockName);
        
        // Save statistics to file
        void SaveToFile() const;
        
        // Get the output file path
        std::string GetOutputPath() const;
        
        // Clear all statistics
        void Clear();
        
        // Get total number of unique blocks tracked
        size_t GetUniqueBlockCount() const { return m_blockCounts.size(); }
        
        // Get total number of conversions
        size_t GetTotalConversions() const;
        
    private:
        UnimplementedBlockTracker() = default;
        ~UnimplementedBlockTracker() = default;
        
        // Block name -> count mapping
        std::unordered_map<std::string, size_t> m_blockCounts;
        mutable std::mutex m_mutex; // Thread safety for tracking
    };

    // Registry for converting Minecraft block names to our internal BlockID enum
    class BlockStateRegistry {
    public:
        static void Initialize();
        static BlockID ResolveBlockState(const NbtBlockState& state);
        static NbtBlockState CreateBlockState(const std::string& name,
                                         const std::unordered_map<std::string, std::string>& props = {});

    private:
        static std::unordered_map<std::string, BlockID> s_nameToBlockId;
        static std::unordered_map<std::string, BlockID> s_stateToBlockId; // For specific states
        static std::once_flag s_initFlag;

        static std::string NormalizeName(const std::string& name);
    };

    // Section data unpacker - converts Minecraft section NBT to our chunk format
    class SectionDataUnpacker {
    public:
        // Main entry point: unpack all sections from a chunk's NBT data
        // Use ::World::NBTTagPtr to refer to the global World namespace
        static bool UnpackChunkSections(const ::World::NBTTagPtr& chunkNBT, Chunk& chunk);

        // Unpack a single section from NBT
        static bool UnpackSection(const ::World::NBTTagPtr& sectionNBT, Chunk& chunk, int sectionY);

        // Parse palette from section NBT
        static std::vector<NbtBlockState> ParsePalette(const ::World::NBTTagPtr& paletteList);

        // Unpack packed block data using palette
        static bool UnpackBlockData(const std::vector<uint64_t>& packedData,
                                   const std::vector<NbtBlockState>& palette,
                                   Chunk& chunk, int sectionY);

        // **NEW**: Unpack block data for Minecraft 1.16+ format (no cross-boundary indices)
        static bool UnpackBlockDataPost116(const std::vector<uint64_t>& packedData,
                                          const std::vector<NbtBlockState>& palette,
                                          ChunkSection& section,
                                          int bitsPerBlock, int sectionY);

        // Calculate bits per block for a palette
        static int CalculateBitsPerBlock(size_t paletteSize);

        // Extract a value from packed long array (legacy method for pre-1.16)
        static uint64_t ExtractPackedValue(const std::vector<uint64_t>& data,
                                         int bitIndex, int bitsPerBlock);

        // Convert block index to local coordinates within section
        static void IndexToCoords(int blockIndex, int& x, int& y, int& z);

        // Convert section-relative coordinates to world coordinates
        static void SectionToWorldCoords(int sectionX, int sectionY, int sectionZ,
                                       int sectionYLevel, int chunkX, int chunkZ,
                                       int& worldX, int& worldY, int& worldZ);

        // Debug: print section statistics
        static void PrintSectionStats(const std::vector<NbtBlockState>& palette,
                                     int sectionY, int blockCount);

    private:
        SectionDataUnpacker() = delete; // Static utility class
    };

} // namespace Game