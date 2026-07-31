// File: src/common/world/chunk/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "../math/WorldMath.hpp"
#include "../math/WorldCoordinates.hpp"
#include "../block/Blocks.hpp"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <functional>
#include <unordered_map>

namespace Game {

    class BlockEntity;

    // Hash for glm::ivec3 — used as the BE map key (local chunk coordinates,
    // with worldY since BEs straddle section boundaries for chest pairing).
    struct IVec3Hash {
        size_t operator()(const glm::ivec3& v) const noexcept {
            // Bit-packed mix; chunk-local extent is well under 2^10 each so
            // collisions are essentially zero for our usage.
            const uint64_t a = static_cast<uint32_t>(v.x);
            const uint64_t b = static_cast<uint32_t>(v.y);
            const uint64_t c = static_cast<uint32_t>(v.z);
            uint64_t h = a * 0x9E3779B97F4A7C15ULL;
            h ^= b + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
            h ^= c + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
            return static_cast<size_t>(h);
        }
    };

    class Chunk {
    public:
        // Chunk position in world chunk coordinates
        Math::ChunkPos pos{0, 0};

        // Array of chunk sections (24 sections of 16x16x16 each)
        std::array<std::unique_ptr<ChunkSection>, Math::SECTIONS_PER_CHUNK> sections;

        // Callback for notifying when a section becomes dirty
        std::function<void(int sectionIndex)> onSectionDirty;

        Chunk();
        // Out-of-line: m_blockEntities holds unique_ptr<BlockEntity> with only
        // a forward declaration above. The destructor must be defined where
        // BlockEntity is complete (Chunk.cpp includes BlockEntity.hpp).
        ~Chunk();

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

        // === BLOCK ENTITY STORAGE ===
        // Mirrors MC LevelChunk.blockEntities: one map per chunk keyed by the
        // BE's LOCAL position (localX, worldY, localZ). World Y is used directly
        // (not section-local Y) so callers can pass the same coords they pass
        // to Get/SetBlock. Created on SetBlock when the new block has a BE type;
        // destroyed on SetBlock when the old block had one.
        //
        // Owning std::unique_ptr — when this Chunk is freed, all its BEs go too.
        // The map is at chunk level (not per-section) because BEs cross section
        // boundaries for queries like chest pairing.

        BlockEntity* GetBlockEntity(int localX, int worldY, int localZ);
        const BlockEntity* GetBlockEntity(int localX, int worldY, int localZ) const;

        // Insert / replace. The chunk takes ownership.
        void SetBlockEntity(int localX, int worldY, int localZ,
                            std::unique_ptr<BlockEntity> entity);

        // Remove and return ownership (or null if absent). Caller may discard
        // to actually delete the BE.
        std::unique_ptr<BlockEntity> RemoveBlockEntity(int localX, int worldY, int localZ);

        // Read-only access to the full BE map for the per-tick walker / chunk
        // packet builder / save serialiser.
        const std::unordered_map<glm::ivec3, std::unique_ptr<BlockEntity>, IVec3Hash>&
        GetAllBlockEntities() const { return m_blockEntities; }

        std::unordered_map<glm::ivec3, std::unique_ptr<BlockEntity>, IVec3Hash>&
        MutableBlockEntities() { return m_blockEntities; }

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

        // Allow move construction and assignment. Same out-of-line constraint
        // as ~Chunk() — defaulted moves of the BE map would need BlockEntity
        // complete here, but Chunk.cpp has the full include so they can be
        // defaulted there.
        Chunk(Chunk&&) noexcept;
        Chunk& operator=(Chunk&&) noexcept;

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

        // Per-cell BlockEntity storage. Keyed by (localX, worldY, localZ).
        std::unordered_map<glm::ivec3, std::unique_ptr<BlockEntity>, IVec3Hash> m_blockEntities;
    };

} // namespace Game