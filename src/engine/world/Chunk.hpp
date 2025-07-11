// File: src/engine/world/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "../../game/WorldMath.hpp"
#include "../block/Blocks.hpp"
#include "../../core/Config.hpp"
#include <array>
#include <memory>
#include <functional>

namespace Game {

    class Chunk {
    public:
        // Chunk position in world chunk coordinates
        Math::ChunkPos pos{0, 0};

        // Array of chunk sections (24 sections of 16x16x16 each)
        std::array<std::unique_ptr<ChunkSection>, Math::SECTIONS_PER_CHUNK> sections;

        // **NEW**: Callback for notifying when a section becomes dirty
        std::function<void(int sectionIndex)> onSectionDirty;

        Chunk();
        ~Chunk() = default;

        // Block access (local X/Z coordinates, world Y coordinate)
        BlockID GetBlock(int localX, int worldY, int localZ) const;
        void SetBlock(int localX, int worldY, int localZ, BlockID blockId);

        // Section management
        ChunkSection* GetSection(int sectionIndex);
        const ChunkSection* GetSection(int sectionIndex) const;
        void EnsureSection(int sectionIndex);
        bool HasSection(int sectionIndex) const;

        // Utility functions
        bool IsValidLocalPosition(int localX, int worldY, int localZ) const;
        int WorldYToSectionIndex(int worldY) const;

        // **FIXED**: Use world Y coordinates consistently
        bool IsWithinChunkBounds(int localX, int worldY, int localZ) const;

        // Statistics
        size_t GetBlockCount() const;
        size_t GetNonAirBlockCount() const;
        bool IsEmpty() const;

        // Constants (chunk-local bounds only)
        static constexpr int SIZE_X = Math::CHUNK_SIZE_X;      // 16
        static constexpr int SIZE_Z = Math::CHUNK_SIZE_Z;      // 16
        static constexpr int SECTION_HEIGHT = Math::SECTION_HEIGHT; // 16
        static constexpr int SECTION_COUNT = Math::SECTIONS_PER_CHUNK; // 24

        // World Y coordinate constants (no more local Y!)
        static constexpr int MIN_WORLD_Y = Config::MinY;         // -64
        static constexpr int MAX_WORLD_Y = Config::MaxY;         // 319
        static constexpr int TOTAL_HEIGHT = MAX_WORLD_Y - MIN_WORLD_Y + 1; // 384 blocks tall

    private:
        // Helper to convert world Y to section coordinates
        void WorldYToSectionCoords(int worldY, int& sectionIndex, int& sectionY) const;
    };

} // namespace Game