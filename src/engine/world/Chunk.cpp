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

    // Block access (local coordinates within chunk)
    BlockID Chunk::GetBlock(int localX, int localY, int localZ) const {
        if (!IsWithinChunkBounds(localX, localY, localZ)) {
            return BlockID::Air;
        }

        int sectionIndex = localY / SECTION_HEIGHT;
        int sectionY = localY % SECTION_HEIGHT;

        const ChunkSection* section = GetSection(sectionIndex);
        if (!section) {
            return BlockID::Air; // Section doesn't exist = all air
        }

        return section->GetBlockID(localX, sectionY, localZ);
    }

    void Chunk::SetBlock(int localX, int localY, int localZ, BlockID blockId) {
        if (!IsWithinChunkBounds(localX, localY, localZ)) {
            Log::Warning("Attempted to set block at invalid local position (%d, %d, %d) in chunk (%d, %d)",
                        localX, localY, localZ, pos.x, pos.z);
            return;
        }

        int sectionIndex = localY / SECTION_HEIGHT;
        int sectionY = localY % SECTION_HEIGHT;

        // Get the old block to check if we're actually changing anything
        BlockID oldBlockId = GetBlock(localX, localY, localZ);
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

            // Mark section as dirty for mesh rebuilding
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

    // Utility functions
    bool Chunk::IsWithinChunkBounds(int localX, int localY, int localZ) const {
        return localX >= 0 && localX < SIZE_X &&
               localY >= 0 && localY < (SECTION_COUNT * SECTION_HEIGHT) &&
               localZ >= 0 && localZ < SIZE_Z;
    }

    // Statistics
    size_t Chunk::GetBlockCount() const {
        return SIZE_X * (SECTION_COUNT * SECTION_HEIGHT) * SIZE_Z;
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
    void Chunk::LocalYToSectionCoords(int sectionLocalY, int& sectionIndex, int& sectionY) const {
        // Input is chunk-local Y (0 to 383), output section index and section-local Y
        sectionIndex = sectionLocalY / SECTION_HEIGHT;
        sectionY = sectionLocalY % SECTION_HEIGHT;

        // Clamp to valid ranges
        sectionIndex = std::clamp(sectionIndex, 0, SECTION_COUNT - 1);
        sectionY = std::clamp(sectionY, 0, SECTION_HEIGHT - 1);
    }

} // namespace Game