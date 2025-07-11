// File: src/engine/world/Chunk.cpp
#include "Chunk.hpp"
#include "../../core/Log.hpp"
#include <algorithm>

namespace Game {

    Chunk::Chunk() {
        // Initialize all sections as null (they'll be created on demand)
        for (auto& section : sections) {
            section = nullptr;
        }
    }

    // Block access (local X/Z coordinates, world Y coordinate)
    BlockID Chunk::GetBlock(int localX, int worldY, int localZ) const {
        if (!IsWithinChunkBounds(localX, worldY, localZ)) {
            return BlockID::Air;
        }

        // Convert world Y to section coordinates
        int sectionIndex = (worldY - Config::MinY) / SECTION_HEIGHT;
        int sectionY = (worldY - Config::MinY) % SECTION_HEIGHT;

        const ChunkSection* section = GetSection(sectionIndex);
        if (!section) {
            return BlockID::Air; // Section doesn't exist = all air
        }

        return section->GetBlockID(localX, sectionY, localZ);
    }

    void Chunk::SetBlock(int localX, int worldY, int localZ, BlockID blockId) {
        if (!IsWithinChunkBounds(localX, worldY, localZ)) {
            Log::Warning("Attempted to set block at invalid position (%d, %d, %d) in chunk (%d, %d)",
                        localX, worldY, localZ, pos.x, pos.z);
            return;
        }

        // Convert world Y to section coordinates
        int sectionIndex = (worldY - Config::MinY) / SECTION_HEIGHT;
        int sectionY = (worldY - Config::MinY) % SECTION_HEIGHT;

        // Get the old block to check if we're actually changing anything
        BlockID oldBlockId = GetBlock(localX, worldY, localZ);
        if (oldBlockId == blockId) {
            return; // No change needed
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

            // **CRITICAL FIX**: Mark section as dirty for mesh rebuilding
            if (onSectionDirty) {
                onSectionDirty(sectionIndex);
            }
        }
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

    // **FIXED**: Use world Y coordinates consistently
    bool Chunk::IsWithinChunkBounds(int localX, int worldY, int localZ) const {
        return localX >= 0 && localX < SIZE_X &&
               worldY >= MIN_WORLD_Y && worldY <= MAX_WORLD_Y &&
               localZ >= 0 && localZ < SIZE_Z;
    }

    // **NEW**: Validation method for external use
    bool Chunk::IsValidLocalPosition(int localX, int worldY, int localZ) const {
        return IsWithinChunkBounds(localX, worldY, localZ);
    }

    // **NEW**: Convert world Y to section index
    int Chunk::WorldYToSectionIndex(int worldY) const {
        return (worldY - Config::MinY) / SECTION_HEIGHT;
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

    // Private helper functions
    void Chunk::WorldYToSectionCoords(int worldY, int& sectionIndex, int& sectionY) const {
        // Input is world Y (-64 to 319), output section index and section-local Y
        int chunkLocalY = worldY - Config::MinY;
        sectionIndex = chunkLocalY / SECTION_HEIGHT;
        sectionY = chunkLocalY % SECTION_HEIGHT;

        // Clamp to valid ranges
        sectionIndex = std::clamp(sectionIndex, 0, SECTION_COUNT - 1);
        sectionY = std::clamp(sectionY, 0, SECTION_HEIGHT - 1);
    }

} // namespace Game