#pragma once

#include <array>
#include <memory>
#include <atomic>
#include <cstdint>
#include "WorldMath.hpp"     // for SECTIONS_PER_CHUNK, ChunkPos
#include "ChunkSection.hpp"  // our section type

namespace Game {

    // Forward‐declare MesherJob’s MeshData so users of Chunk don’t need to include Mesher.hpp.
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

        // Convenience: return the section index [0..15] from a global Y: (globalY / SECTION_HEIGHT).
        inline static int SectionIndexFromGlobalY(int globalY) {
            // Each section spans 16 in Y. floor(globalY mod 256 / 16).
            // If you store only relative Y [0..255], do: (globalY % 256) / 16.
            // But typically you’ll pass local section index directly.
            return (globalY / Math::SECTION_HEIGHT) % Math::SECTIONS_PER_CHUNK;
        }

        // Access a block within this chunk by (localX, localY, localZ).
        // localX ∈ [0..15], localY ∈ [0..255], localZ ∈ [0..15].
        inline BlockID GetBlock(int localX, int localY, int localZ) const {
            int sectionIdx = localY / Math::SECTION_HEIGHT;
            int yInSection = localY % Math::SECTION_HEIGHT;
            if (!sections[sectionIdx]) {
                return BlockID::Air; // uninitialized sections default to Air
            }
            return sections[sectionIdx]->GetBlockID(localX, yInSection, localZ);
        }

        // Set a block—if the section doesn’t exist yet, create it.
        inline void SetBlock(int localX, int localY, int localZ, BlockID id) {
            int sectionIdx = localY / Math::SECTION_HEIGHT;
            int yInSection = localY % Math::SECTION_HEIGHT;
            if (!sections[sectionIdx]) {
                sections[sectionIdx] = std::make_unique<ChunkSection>();
            }
            sections[sectionIdx]->Set(localX, yInSection, localZ, id);
            needsMesh.store(true, std::memory_order_relaxed);
        }
    };

} // namespace Game
