// File: src/common/world/chunk/Chunk.cpp
#include "Chunk.hpp"
#include "../biome/Biomes.hpp"
#include "../block/entity/BlockEntity.hpp"
#include "../../core/Log.hpp"
#include "../math/WorldCoordinates.hpp"
#include <algorithm>

namespace Game {

    Chunk::Chunk() {
        // Initialize all sections as null (they'll be created on demand)
        for (auto& section : sections) {
            section = nullptr;
        }
    }

    // Out-of-line so unique_ptr<BlockEntity> destructor sees the complete
    // BlockEntity type (Chunk.hpp only forward-declares it).
    Chunk::~Chunk()                                = default;
    Chunk::Chunk(Chunk&&) noexcept                 = default;
    Chunk& Chunk::operator=(Chunk&&) noexcept      = default;

    // Block access (local X/Z coordinates, world Y coordinate)
    BlockID Chunk::GetBlock(int localX, int worldY, int localZ) const {
        if (!ValidateCoordinates(localX, worldY, localZ, "GetBlock")) {
            return BlockID::Air;
        }

        // **UPDATED**: Use WorldCoordinates for conversion
        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return BlockID::Air;
        }

        const ChunkSection* section = GetSection(sectionIndex);
        if (!section) {
            return BlockID::Air; // Section doesn't exist = all air
        }

        return section->GetBlockID(localX, sectionY, localZ);
    }

    // MC ChunkAccess.getNoiseBiome — the caller's block coordinates are shifted
    // right by 2 and clamped into the chunk's quarter grid.
    // Biomes live on the SECTION now (MC LevelChunkSection.biomes). These two
    // keep the chunk-level, world-Y vocabulary the rest of the engine uses and
    // route it to the right section's 4x4x4 container.
    uint16_t Chunk::GetBiome(int localX, int worldY, int localZ) const {
        const int qxAll = std::clamp(localX >> 2, 0, BIOME_HORIZONTAL - 1);
        const int qzAll = std::clamp(localZ >> 2, 0, BIOME_HORIZONTAL - 1);
        // Y is world-space and starts at MIN_WORLD_Y (-64), so it has to be
        // rebased before the shift or everything below y=0 lands in cell 0.
        const int qyAll = std::clamp(
            (worldY - Math::WorldCoordinates::MIN_WORLD_Y) >> 2, 0, BIOME_VERTICAL - 1);

        const int sectionIndex = qyAll / ChunkSection::BIOME_AXIS;
        const ChunkSection* section =
            (sectionIndex >= 0 && sectionIndex < SECTION_COUNT) ? GetSection(sectionIndex) : nullptr;
        if (!section) return kFallbackBiomeId;

        return section->GetBiome(qxAll, qyAll % ChunkSection::BIOME_AXIS, qzAll);
    }

    void Chunk::SetBiomeQuart(int qx, int qy, int qz, uint16_t biomeId) {
        if (qx < 0 || qx >= BIOME_HORIZONTAL || qz < 0 || qz >= BIOME_HORIZONTAL ||
            qy < 0 || qy >= BIOME_VERTICAL) {
            return;
        }
        const int sectionIndex = qy / ChunkSection::BIOME_AXIS;
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) return;
        EnsureSection(sectionIndex);
        if (ChunkSection* section = GetSection(sectionIndex)) {
            section->SetBiome(qx, qy % ChunkSection::BIOME_AXIS, qz, biomeId);
        }
    }

    uint8_t Chunk::GetBlockState(int localX, int worldY, int localZ) const {
        if (!ValidateCoordinates(localX, worldY, localZ, "GetBlockState")) {
            return 0;
        }

        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return 0;
        }

        const ChunkSection* section = GetSection(sectionIndex);
        if (!section) {
            return 0; // Section doesn't exist = all air = default state
        }

        return section->GetState(localX, sectionY, localZ);
    }

    void Chunk::SetBlock(int localX, int worldY, int localZ, BlockID blockId, uint8_t stateIndex) {
        if (!ValidateCoordinates(localX, worldY, localZ, "SetBlock")) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d) in chunk (%d, %d)",
                        localX, worldY, localZ, pos.x, pos.z);
            return;
        }

        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            Log::Warning("Invalid section index %d for world Y %d in chunk (%d, %d)",
                        sectionIndex, worldY, pos.x, pos.z);
            return;
        }

        // Deliberately NOT short-circuiting on "same BlockID" the way the
        // BlockID-only overload does: re-orienting a block (same id, different
        // state) is a real change and must still write and dirty the section.
        const BlockID oldBlockId = GetBlock(localX, worldY, localZ);
        const uint8_t oldState   = GetBlockState(localX, worldY, localZ);
        if (oldBlockId == blockId && oldState == stateIndex) {
            return;
        }

        if (blockId == BlockID::Air && stateIndex == 0 && !HasSection(sectionIndex)) {
            return;
        }

        if (blockId != BlockID::Air) {
            EnsureSection(sectionIndex);
        }

        ChunkSection* section = GetSection(sectionIndex);
        if (section) {
            section->Set(localX, sectionY, localZ, blockId);
            section->SetState(localX, sectionY, localZ, stateIndex);

            UpdateHeightmaps(localX, worldY, localZ, blockId);

            if (onSectionDirty) {
                onSectionDirty(sectionIndex);
            }
        }
    }

    void Chunk::SetBlock(int localX, int worldY, int localZ, BlockID blockId) {
        if (!ValidateCoordinates(localX, worldY, localZ, "SetBlock")) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d) in chunk (%d, %d)",
                        localX, worldY, localZ, pos.x, pos.z);
            return;
        }

        // **UPDATED**: Use WorldCoordinates for conversion
        int sectionIndex, sectionY;
        Math::WorldCoordinates::WorldYToSectionCoords(worldY, sectionIndex, sectionY);

        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            Log::Warning("Invalid section index %d for world Y %d in chunk (%d, %d)",
                        sectionIndex, worldY, pos.x, pos.z);
            return;
        }

        // Get the old block to check if we're actually changing anything
        BlockID oldBlockId = GetBlock(localX, worldY, localZ);
        if (oldBlockId == blockId) {
            return; // No change needed — leaves any existing state untouched
        }

        // If setting air and section doesn't exist, no need to create it
        if (blockId == BlockID::Air && !HasSection(sectionIndex)) {
            return;
        }

        // Ensure section exists for non-air blocks
        if (blockId != BlockID::Air) {
            EnsureSection(sectionIndex);
        }

        ChunkSection* section = GetSection(sectionIndex);
        if (section) {
            section->Set(localX, sectionY, localZ, blockId);

            // The block genuinely changed, so any state left over from the
            // previous occupant is meaningless — state indices are relative to
            // the owning block's own state list. Reset to the new block's
            // default (MC defaultBlockState()). Without this, mining a
            // west-facing furnace and placing stone would leave stone carrying
            // state index 3.
            section->SetState(localX, sectionY, localZ, 0);

            UpdateHeightmaps(localX, worldY, localZ, blockId);

            // Mark section as dirty for mesh rebuilding
            if (onSectionDirty) {
                onSectionDirty(sectionIndex);
            }
        }
    }

    void Chunk::UpdateHeightmaps(int localX, int worldY, int localZ, BlockID newBlock) {
        // MC LevelChunk.setBlockState updates every heightmap on every write.
        // Skipped entirely until the chunk is primed: before that the stored
        // heights are all minY, so an Update would happily conclude that the
        // first block written is the surface and leave every column below it
        // wrong. The prime pass is what establishes the truth.
        if (!m_heightmapsPrimed) return;

        const auto blockAt = [this](int x, int y, int z) { return GetBlock(x, y, z); };

        for (size_t i = 0; i < static_cast<size_t>(HeightmapType::Count); ++i) {
            m_heightmaps[i].Update(localX, worldY, localZ, newBlock,
                                   static_cast<HeightmapType>(i), blockAt);
        }
    }

    void Chunk::PrimeHeightmaps() {
        // MC Heightmap.primeHeightmaps: one downward scan per column, stopping
        // as soon as every map has found its surface.
        //
        // This is the SLOW path and it is meant to be — a freshly generated
        // chunk copies the terrain library's already-computed heights instead
        // (see MyTerrainGenerator), and a loaded chunk reads them from NBT.
        // Reaching here means neither was available.
        constexpr size_t kTypeCount = static_cast<size_t>(HeightmapType::Count);

        // Start from the top of the highest non-empty section rather than the
        // build limit: most chunks are empty above y=128 and scanning that is
        // pure waste.
        int scanTop = MIN_WORLD_Y;
        for (int i = SECTION_COUNT - 1; i >= 0; --i) {
            if (HasSection(i)) {
                scanTop = Math::WorldCoordinates::SectionCoordsToWorldY(i, SECTION_HEIGHT - 1);
                break;
            }
        }

        for (size_t i = 0; i < kTypeCount; ++i) m_heightmaps[i].Reset(MIN_WORLD_Y);

        for (int x = 0; x < SIZE_X; ++x) {
            for (int z = 0; z < SIZE_Z; ++z) {
                bool found[kTypeCount] = {};
                size_t remaining = kTypeCount;

                for (int y = scanTop; y >= MIN_WORLD_Y && remaining > 0; --y) {
                    const BlockID block = GetBlock(x, y, z);
                    if (block == BlockID::Air) continue;

                    for (size_t i = 0; i < kTypeCount; ++i) {
                        if (found[i]) continue;
                        if (!HeightmapIsOpaque(static_cast<HeightmapType>(i), block)) continue;
                        m_heightmaps[i].SetHeight(x, z, y + 1);
                        found[i] = true;
                        --remaining;
                    }
                }
            }
        }

        m_heightmapsPrimed = true;
    }

    // Section management
    ChunkSection* Chunk::GetSection(int sectionIndex) {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return nullptr;
        }
        return sections[sectionIndex].get();
    }

    const ChunkSection* Chunk::GetSection(int sectionIndex) const {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return nullptr;
        }
        return sections[sectionIndex].get();
    }

    void Chunk::EnsureSection(int sectionIndex) {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            Log::Warning("Invalid section index %d in chunk (%d, %d)", sectionIndex, pos.x, pos.z);
            return;
        }

        if (!sections[sectionIndex]) {
            sections[sectionIndex] = std::make_unique<ChunkSection>();
        }
    }

    bool Chunk::HasSection(int sectionIndex) const {
        if (sectionIndex < 0 || sectionIndex >= SECTION_COUNT) {
            return false;
        }
        return sections[sectionIndex] != nullptr;
    }

    // ========================================================================
    // BLOCK ENTITY STORAGE
    // ========================================================================

    BlockEntity* Chunk::GetBlockEntity(int localX, int worldY, int localZ) {
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        return (it != m_blockEntities.end()) ? it->second.get() : nullptr;
    }

    const BlockEntity* Chunk::GetBlockEntity(int localX, int worldY, int localZ) const {
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        return (it != m_blockEntities.end()) ? it->second.get() : nullptr;
    }

    void Chunk::SetBlockEntity(int localX, int worldY, int localZ,
                                std::unique_ptr<BlockEntity> entity) {
        if (!entity) {
            m_blockEntities.erase(glm::ivec3(localX, worldY, localZ));
            return;
        }
        m_blockEntities[glm::ivec3(localX, worldY, localZ)] = std::move(entity);
    }

    std::unique_ptr<BlockEntity>
    Chunk::RemoveBlockEntity(int localX, int worldY, int localZ) {
        auto it = m_blockEntities.find(glm::ivec3(localX, worldY, localZ));
        if (it == m_blockEntities.end()) return nullptr;
        std::unique_ptr<BlockEntity> out = std::move(it->second);
        m_blockEntities.erase(it);
        return out;
    }

    // Statistics
    size_t Chunk::GetBlockCount() const {
        return SIZE_X * TOTAL_HEIGHT * SIZE_Z;
    }

    size_t Chunk::GetNonAirBlockCount() const {
        size_t count = 0;

        for (int sectionIndex = 0; sectionIndex < SECTION_COUNT; ++sectionIndex) {
            const ChunkSection* section = GetSection(sectionIndex);
            if (!section) {
                continue; // Null section = all air = 0 non-air blocks
            }

            // Count non-air blocks in this section
            for (int x = 0; x < ChunkSection::SIZE; ++x) {
                for (int y = 0; y < ChunkSection::SIZE; ++y) {
                    for (int z = 0; z < ChunkSection::SIZE; ++z) {
                        if (section->GetBlockID(x, y, z) != BlockID::Air) {
                            count++;
                        }
                    }
                }
            }
        }

        return count;
    }

    bool Chunk::IsEmpty() const {
        // Check if all sections are null (which means all air)
        for (const auto& section : sections) {
            if (section != nullptr) {
                return false;
            }
        }
        return true;
    }

    // **NEW**: Helper method for coordinate validation with detailed logging
    bool Chunk::ValidateCoordinates(int localX, int worldY, int localZ, const char* operation) const {
        if (localX < 0 || localX >= SIZE_X) {
            Log::Warning("%s: Invalid localX %d (must be 0-%d) in chunk (%d, %d)",
                        operation, localX, SIZE_X - 1, pos.x, pos.z);
            return false;
        }

        if (localZ < 0 || localZ >= SIZE_Z) {
            Log::Warning("%s: Invalid localZ %d (must be 0-%d) in chunk (%d, %d)",
                        operation, localZ, SIZE_Z - 1, pos.x, pos.z);
            return false;
        }

        if (!Math::WorldCoordinates::IsValidWorldY(worldY)) {
            Log::Warning("%s: Invalid worldY %d (must be %d-%d) in chunk (%d, %d)",
                        operation, worldY, MIN_WORLD_Y, MAX_WORLD_Y, pos.x, pos.z);
            return false;
        }

        return true;
    }

} // namespace Game