#pragma once

#include <array>
#include <memory>
#include <atomic>
#include <cstdint>
#include "WorldMath.hpp"     // for SECTIONS_PER_CHUNK, ChunkPos
#include "ChunkSection.hpp"  // our section type
#include "../core/Config.hpp" // for MinY

namespace Game {

    // Forward‐declare MesherJob's MeshData so users of Chunk don't need to include Mesher.hpp.
    struct MeshData;

    class Chunk {
    public:
        // Which chunk (in XZ plane) this represents
        Math::ChunkPos pos{ 0, 0 };

        // Each chunk is subdivided vertically into 16 sections (16×16×16 voxels each).
        // We use unique_ptr so that we can lazily allocate or drop sections as needed.
        std::array<std::unique_ptr<ChunkSection>, Math::SECTIONS_PER_CHUNK> sections{};

        // Flags for meshing state.
        // - needsMesh = true when either: newly created OR world data changed in a section.
        // - hasMesh = true when a MeshData has already been built and enqueued for GPU upload.
        std::atomic<bool> needsMesh{ false };
        std::atomic<bool> hasMesh{ false };

        Chunk() = default;

        // FIXED: Convert world Y to section index, properly handling negative coordinates
        inline static int SectionIndexFromGlobalY(int globalY) {
            // Adjust for MinY offset and convert to section index
            int adjustedY = globalY - Config::MinY;  // Convert to 0-based indexing
            if (adjustedY < 0) {
                return -1; // Below world bounds
            }
            int sectionIndex = adjustedY / Math::SECTION_HEIGHT;
            if (sectionIndex >= Math::SECTIONS_PER_CHUNK) {
                return -1; // Above world bounds
            }
            return sectionIndex;
        }

        // FIXED: Convert world Y to local Y within section, properly handling negative coordinates
        inline static int LocalYFromGlobalY(int globalY) {
            int adjustedY = globalY - Config::MinY;  // Convert to 0-based indexing
            return adjustedY % Math::SECTION_HEIGHT;
        }

        // Access a block within this chunk by (localX, localY, localZ).
        // localX ∈ [0..15], localY ∈ [Config::MinY..Config::MaxY], localZ ∈ [0..15].
        // FIXED: Proper handling of world Y coordinates
        inline BlockID GetBlock(int localX, int worldY, int localZ) const {
            int sectionIdx = SectionIndexFromGlobalY(worldY);
            if (sectionIdx < 0 || sectionIdx >= Math::SECTIONS_PER_CHUNK) {
                return BlockID::Air; // Out of world bounds
            }

            int yInSection = LocalYFromGlobalY(worldY);
            if (!sections[sectionIdx]) {
                return BlockID::Air; // uninitialized sections default to Air
            }
            return sections[sectionIdx]->GetBlockID(localX, yInSection, localZ);
        }

        // Set a block—if the section doesn't exist yet, create it.
        // FIXED: Proper handling of world Y coordinates
        inline void SetBlock(int localX, int worldY, int localZ, BlockID id) {
            int sectionIdx = SectionIndexFromGlobalY(worldY);
            if (sectionIdx < 0 || sectionIdx >= Math::SECTIONS_PER_CHUNK) {
                return; // Out of world bounds, ignore
            }

            int yInSection = LocalYFromGlobalY(worldY);
            if (!sections[sectionIdx]) {
                sections[sectionIdx] = std::make_unique<ChunkSection>();
            }
            sections[sectionIdx]->Set(localX, yInSection, localZ, id);
            needsMesh.store(true, std::memory_order_relaxed);
        }
    };

} // namespace Game