// File: src/common/world/chunk/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "../math/WorldMath.hpp"
#include "../math/WorldCoordinates.hpp"
#include "../block/Blocks.hpp"
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

        // **FIXED**: Added missing method that Mesher was trying to call
        bool IsWithinChunkBounds(int localX, int worldY, int localZ) const {
            return localX >= 0 && localX < SIZE_X &&
                   Math::WorldCoordinates::IsValidWorldY(worldY) &&
                   localZ >= 0 && localZ < SIZE_Z;
        }

        // Validate local coordinates (uses WorldCoordinates internally)
        bool IsValidLocalPosition(int localX, int worldY, int localZ) const {
            return IsWithinChunkBounds(localX, worldY, localZ);
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

        // **NEW**: Get count of non-null sections
        size_t GetSectionCount() const {
            size_t count = 0;
            for (const auto& section : sections) {
                if (section != nullptr) {
                    count++;
                }
            }
            return count;
        }

        // Delete copy constructor and assignment to prevent accidental copies
        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;

        // Allow move construction and assignment
        Chunk(Chunk&&) = default;
        Chunk& operator=(Chunk&&) = default;

        // Create a deep copy of this chunk
        std::shared_ptr<Chunk> Clone() const {
            auto cloned = std::make_shared<Chunk>();
            cloned->pos = this->pos;

            // Deep copy all sections
            for (int i = 0; i < SECTION_COUNT; ++i) {
                if (this->HasSection(i)) {
                    cloned->EnsureSection(i);
                    const ChunkSection* srcSection = this->GetSection(i);
                    ChunkSection* dstSection = cloned->GetSection(i);

                    // Copy the blocks array and palette
                    dstSection->blocks = srcSection->blocks;
                    dstSection->palette = srcSection->palette;
                }
            }

            return cloned;
        }

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
        // Helper to validate coordinates and log errors
        bool ValidateCoordinates(int localX, int worldY, int localZ, const char* operation) const;
    };

} // namespace Game