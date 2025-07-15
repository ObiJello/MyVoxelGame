// File: src/engine/world/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "../../game/WorldMath.hpp"
#include "../../game/WorldCoordinates.hpp"  // **NEW**: Use centralized coordinates
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

        // Callback for notifying when a section becomes dirty
        std::function<void(int sectionIndex)> onSectionDirty;

        Chunk();
        ~Chunk() = default;

        // === BLOCK ACCESS (using world Y coordinates) ===

        // Get block at local chunk coordinates with world Y
        BlockID GetBlock(int localX, int worldY, int localZ) const;

        // Set block at local chunk coordinates with world Y
        void SetBlock(int localX, int worldY, int localZ, BlockID blockId);

        // === SECTION MANAGEMENT ===

        // Get mutable section by index
        ChunkSection* GetSection(int sectionIndex);

        // Get immutable section by index
        const ChunkSection* GetSection(int sectionIndex) const;

        // Ensure section exists (create if needed)
        void EnsureSection(int sectionIndex);

        // Check if section exists
        bool HasSection(int sectionIndex) const;

        // === COORDINATE UTILITIES ===

        // Validate local coordinates (uses WorldCoordinates internally)
        bool IsValidLocalPosition(int localX, int worldY, int localZ) const {
            return localX >= 0 && localX < SIZE_X &&
                   Math::WorldCoordinates::IsValidWorldY(worldY) &&
                   localZ >= 0 && localZ < SIZE_Z;
        }

        // Convert world Y to section index (delegates to WorldCoordinates)
        int WorldYToSectionIndex(int worldY) const {
            return Math::WorldCoordinates::WorldYToSectionIndex(worldY);
        }

        // === STATISTICS ===

        // Get total possible blocks in chunk
        size_t GetBlockCount() const;

        // Count non-air blocks
        size_t GetNonAirBlockCount() const;

        // Check if chunk is completely empty (all air)
        bool IsEmpty() const;

        // === CONSTANTS ===

        static constexpr int SIZE_X = Math::CHUNK_SIZE_X;      // 16
        static constexpr int SIZE_Z = Math::CHUNK_SIZE_Z;      // 16
        static constexpr int SECTION_HEIGHT = Math::SECTION_HEIGHT; // 16
        static constexpr int SECTION_COUNT = Math::SECTIONS_PER_CHUNK; // 24

        // World Y coordinate constants
        static constexpr int MIN_WORLD_Y = Math::WorldCoordinates::MIN_WORLD_Y;         // -64
        static constexpr int MAX_WORLD_Y = Math::WorldCoordinates::MAX_WORLD_Y;         // 319
        static constexpr int TOTAL_HEIGHT = Math::WorldCoordinates::WORLD_HEIGHT;       // 384

    private:
        // **REMOVED**: Duplicate Y conversion methods - now use WorldCoordinates
        // **REMOVED**: WorldYToSectionCoords() - use Math::WorldCoordinates::WorldYToSectionCoords()
        // **REMOVED**: IsWithinChunkBounds() - use IsValidLocalPosition()

        // Helper to validate coordinates and log errors
        bool ValidateCoordinates(int localX, int worldY, int localZ, const char* operation) const;
    };

} // namespace Game