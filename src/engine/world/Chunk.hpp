// File: src/engine/world/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "../../game/WorldMath.hpp"
#include "../block/Blocks.hpp"
#include "../../core/Config.hpp"
#include <array>
#include <memory>

namespace Game {

    class Chunk {
    public:
        // Chunk position in world chunk coordinates
        Math::ChunkPos pos{0, 0};

        // Array of chunk sections (24 sections of 16x16x16 each)
        std::array<std::unique_ptr<ChunkSection>, Math::SECTIONS_PER_CHUNK> sections;

        Chunk();
        ~Chunk() = default;

        // Block access (local coordinates within chunk)
        BlockID GetBlock(int localX, int localY, int localZ) const;
        void SetBlock(int localX, int localY, int localZ, BlockID blockId);

        // Section management
        ChunkSection* GetSection(int sectionIndex);
        const ChunkSection* GetSection(int sectionIndex) const;
        void EnsureSection(int sectionIndex);
        bool HasSection(int sectionIndex) const;

        // Utility functions
        bool IsValidLocalPosition(int localX, int localY, int localZ) const;
        int WorldYToSectionIndex(int worldY) const;
        int WorldYToLocalY(int worldY) const;
        bool IsWithinChunkBounds(int localX, int localY, int localZ) const;

        // Statistics
        size_t GetBlockCount() const;
        size_t GetNonAirBlockCount() const;
        bool IsEmpty() const;

        // Constants (chunk-local bounds only)
        static constexpr int SIZE_X = Math::CHUNK_SIZE_X;      // 16
        static constexpr int SIZE_Z = Math::CHUNK_SIZE_Z;      // 16
        static constexpr int SECTION_HEIGHT = Math::SECTION_HEIGHT; // 16
        static constexpr int SECTION_COUNT = Math::SECTIONS_PER_CHUNK; // 24

    private:
        // Helper to convert section-local Y to section coordinates
        void LocalYToSectionCoords(int sectionLocalY, int& sectionIndex, int& sectionY) const;
    };

} // namespace Game