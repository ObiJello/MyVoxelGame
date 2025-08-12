// File: src/common/world/chunk/Chunk.cpp
#include "Chunk.hpp"
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