// File: src/common/world/chunk/Chunk.hpp
#pragma once

#include "ChunkSection.hpp"
#include "Heightmap.hpp"
#include "../math/WorldMath.hpp"
#include "../math/WorldCoordinates.hpp"
#include "../block/Blocks.hpp"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>

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

        // === BIOMES ===
        //
        // MC stores biomes at QUARTER resolution — one entry per 4x4x4 cell —
        // and every lookup goes through
        // `getNoiseBiome(x >> 2, y >> 2, z >> 2)` (ChunkAccess.getNoiseBiome).
        // Matching that resolution is not an optimisation: the 4-block cells
        // are visible at biome borders, and sampling per block would give
        // different colours from vanilla along every edge.
        //
        // Flat 4 x 96 x 4 (x, yQuart, z), indexed low-to-high in Y from
        // Math::MIN_Y. Empty until the chunk is given biome data, in which case
        // every lookup answers with the fallback biome.
        static constexpr int BIOME_HORIZONTAL = 4;                 // 16 blocks / 4
        static constexpr int BIOME_VERTICAL   = Math::SECTIONS_PER_CHUNK * 4;
        static constexpr int BIOME_COUNT      = BIOME_HORIZONTAL * BIOME_VERTICAL * BIOME_HORIZONTAL;
        // Biomes moved onto ChunkSection (MC LevelChunkSection.biomes). The
        // BIOME_* constants stay because GetBiome/SetBiomeQuart still speak in
        // whole-column quart coordinates.

        // Biome at chunk-local block coordinates with world Y. Quantises the
        // same way MC does. Returns the fallback when the chunk carries none.
        uint16_t GetBiome(int localX, int worldY, int localZ) const;
        void SetBiomeQuart(int qx, int qy, int qz, uint16_t biomeId);

        // === HEIGHTMAPS ===
        //
        // MC ChunkAccess.heightmaps. Primed once (see PrimeHeightmaps or the
        // copy path in MyTerrainGenerator) and then kept current by SetBlock,
        // so nothing ever has to scan a column to find the surface.
        //
        // `primed` distinguishes "this column is empty" from "nobody has filled
        // this in yet". A chunk straight from the Anvil loader with no
        // Heightmaps tag is unprimed, and callers must prime it before reading
        // — exactly MC's hasPrimedHeightmap contract.
        Heightmap& GetHeightmap(HeightmapType type) {
            return m_heightmaps[static_cast<size_t>(type)];
        }
        const Heightmap& GetHeightmap(HeightmapType type) const {
            return m_heightmaps[static_cast<size_t>(type)];
        }

        bool AreHeightmapsPrimed() const { return m_heightmapsPrimed; }
        void MarkHeightmapsPrimed() { m_heightmapsPrimed = true; }

        // MC Heightmap.primeHeightmaps — the full column scan. This is the
        // FALLBACK path: it is what a chunk loaded from disk without a stored
        // heightmap needs, and it is deliberately the expensive option. Freshly
        // generated chunks copy the terrain library's already-computed values
        // instead.
        void PrimeHeightmaps();

        // MC ChunkAccess.getHeight — topmost matching block in this column.
        // Chunk-local x/z.
        int GetSurfaceHeight(int localX, int localZ, HeightmapType type) const {
            return m_heightmaps[static_cast<size_t>(type)].GetHeight(localX, localZ);
        }

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

        // === BLOCK STATE ACCESS ===
        // The state index is an index into the block's own state list (MC's
        // BlockState.getId()); 0 is always the default state. Kept as separate
        // overloads rather than extra parameters on GetBlock/SetBlock so the
        // several hundred existing call sites that don't care about state stay
        // untouched.

        uint8_t GetBlockState(int localX, int worldY, int localZ) const;

        // Set block and state together. Unlike SetBlock(...) this proceeds even
        // when the BlockID is unchanged, so a pure re-orientation still applies
        // and still dirties the section.
        void SetBlock(int localX, int worldY, int localZ, BlockID blockId, uint8_t stateIndex);

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

                    // One assignment now: the paletted container carries the
                    // blocks, their states and the censuses together.
                    *dstSection = *srcSection;
                }
            }

            // Heightmaps travel with the copy. Without this a cloned chunk
            // reports every column empty until something writes to it, which
            // would show up as mobs spawning at the world floor.
            cloned->m_heightmaps = this->m_heightmaps;
            cloned->m_heightmapsPrimed = this->m_heightmapsPrimed;

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

        // MC LevelChunk.setBlockState's heightmap pass. Called from both
        // SetBlock overloads; a no-op until the chunk has been primed.
        void UpdateHeightmaps(int localX, int worldY, int localZ, BlockID newBlock);

        // Per-cell BlockEntity storage. Keyed by (localX, worldY, localZ).
        std::unordered_map<glm::ivec3, std::unique_ptr<BlockEntity>, IVec3Hash> m_blockEntities;

        std::array<Heightmap, static_cast<size_t>(HeightmapType::Count)> m_heightmaps;
        bool m_heightmapsPrimed = false;
    };

} // namespace Game