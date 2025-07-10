// File: src/engine/world/Chunk.hpp - Enhanced with Section Mesh Integration
#pragma once

#include <array>
#include <memory>
#include <atomic>
#include <cstdint>
#include <unordered_set>
#include "../../game/WorldMath.hpp"
#include "ChunkSection.hpp"
#include "Log.hpp"
#include "../../core/Config.hpp"

namespace Game {

    // Forward‐declare MesherJob's MeshData so users of Chunk don't need to include Mesher.hpp.
    struct MeshData;

    class Chunk {
    public:
        // Which chunk (in XZ plane) this represents
        Math::ChunkPos pos{ 0, 0 };

        // Each chunk is subdivided vertically into 24 sections (16×16×16 voxels each).
        // We use unique_ptr so that we can lazily allocate or drop sections as needed.
        std::array<std::unique_ptr<ChunkSection>, Math::SECTIONS_PER_CHUNK> sections{};

        // **NEW**: Section-level mesh dirty tracking
        std::atomic<uint32_t> dirtyMeshSections{0}; // Bitmask of sections needing remesh (0-23)
        std::atomic<bool> hasAnyMesh{false};        // Whether any section has a mesh
        std::atomic<int> meshSectionCount{0};       // Number of sections with meshes

        // Legacy flags for backward compatibility
        std::atomic<bool> needsMesh{ false };       // TRUE if any section needs meshing
        std::atomic<bool> hasMesh{ false };         // TRUE if any section has a mesh

        Chunk() = default;

        // FIXED: Convert world Y to section index, properly handling negative coordinates
        inline static int SectionIndexFromGlobalY(int globalY) {
            // Check world bounds first - FIXED: MaxY is inclusive, so use >= for upper bound
            if (globalY < Config::MinY || globalY > Config::MaxY) {
                return -1; // Out of world bounds
            }

            // Adjust for MinY offset and convert to section index
            int adjustedY = globalY - Config::MinY;  // Convert to 0-based indexing
            int sectionIndex = adjustedY / Math::SECTION_HEIGHT;

            // FIXED: The bounds check was wrong here - we have 24 sections (0-23)
            // World Y range is -64 to 319 = 384 blocks total
            // 384 blocks / 16 blocks per section = 24 sections
            // So valid section indices are 0 to 23
            if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) {
                return -1; // Out of bounds
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
        // **ENHANCED**: Now marks specific section for remeshing
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

            // **NEW**: Mark specific section for remeshing
            MarkSectionDirty(sectionIdx);
        }

        // **NEW**: Section-specific mesh management

        // Mark a specific section as needing remesh
        inline void MarkSectionDirty(int sectionIndex) {
            if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                // Set bit for this section
                uint32_t mask = 1u << sectionIndex;
                dirtyMeshSections.fetch_or(mask, std::memory_order_relaxed);

                // Update legacy flag
                needsMesh.store(true, std::memory_order_relaxed);

                Log::Debug("Marked section %d dirty for chunk (%d, %d)",
                          sectionIndex, pos.x, pos.z);
            }
        }

        // Mark multiple sections as needing remesh
        inline void MarkSectionsDirty(const std::unordered_set<int>& sectionIndices) {
            uint32_t combinedMask = 0;
            for (int sectionIndex : sectionIndices) {
                if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                    combinedMask |= (1u << sectionIndex);
                }
            }

            if (combinedMask != 0) {
                dirtyMeshSections.fetch_or(combinedMask, std::memory_order_relaxed);
                needsMesh.store(true, std::memory_order_relaxed);

                Log::Debug("Marked %zu sections dirty for chunk (%d, %d)",
                          sectionIndices.size(), pos.x, pos.z);
            }
        }

        // Check if a specific section needs remeshing
        inline bool IsSectionDirty(int sectionIndex) const {
            if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) {
                return false;
            }

            uint32_t mask = 1u << sectionIndex;
            return (dirtyMeshSections.load(std::memory_order_relaxed) & mask) != 0;
        }

        // Get all sections that need remeshing
        inline std::unordered_set<int> GetDirtySections() const {
            std::unordered_set<int> dirtySections;
            uint32_t dirtyMask = dirtyMeshSections.load(std::memory_order_relaxed);

            for (int i = 0; i < Math::SECTIONS_PER_CHUNK; ++i) {
                if (dirtyMask & (1u << i)) {
                    dirtySections.insert(i);
                }
            }

            return dirtySections;
        }

        // Clear dirty flag for a specific section
        inline void ClearSectionDirty(int sectionIndex) {
            if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                uint32_t mask = ~(1u << sectionIndex); // Invert mask to clear bit
                dirtyMeshSections.fetch_and(mask, std::memory_order_relaxed);

                // Update legacy flags
                UpdateLegacyFlags();

                Log::Debug("Cleared dirty flag for section %d in chunk (%d, %d)",
                          sectionIndex, pos.x, pos.z);
            }
        }

        // Clear dirty flags for multiple sections
        inline void ClearSectionsDirty(const std::unordered_set<int>& sectionIndices) {
            uint32_t clearMask = 0xFFFFFFFF; // Start with all bits set

            for (int sectionIndex : sectionIndices) {
                if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                    clearMask &= ~(1u << sectionIndex); // Clear bit for this section
                }
            }

            dirtyMeshSections.fetch_and(clearMask, std::memory_order_relaxed);
            UpdateLegacyFlags();

            Log::Debug("Cleared dirty flags for %zu sections in chunk (%d, %d)",
                      sectionIndices.size(), pos.x, pos.z);
        }

        // Mark a section as having a mesh
        inline void MarkSectionMeshed(int sectionIndex) {
            if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                // Clear dirty flag and increment mesh count
                ClearSectionDirty(sectionIndex);

                int currentCount = meshSectionCount.fetch_add(1, std::memory_order_relaxed);
                if (currentCount == 0) {
                    hasAnyMesh.store(true, std::memory_order_relaxed);
                    hasMesh.store(true, std::memory_order_relaxed); // Legacy flag
                }

                Log::Debug("Section %d meshed for chunk (%d, %d), total: %d sections",
                          sectionIndex, pos.x, pos.z, currentCount + 1);
            }
        }

        // Mark a section as no longer having a mesh
        inline void MarkSectionUnmeshed(int sectionIndex) {
            if (sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK) {
                int currentCount = meshSectionCount.fetch_sub(1, std::memory_order_relaxed);
                if (currentCount <= 1) {
                    hasAnyMesh.store(false, std::memory_order_relaxed);
                    hasMesh.store(false, std::memory_order_relaxed); // Legacy flag
                }

                Log::Debug("Section %d unmeshed for chunk (%d, %d), remaining: %d sections",
                          sectionIndex, pos.x, pos.z, std::max(0, currentCount - 1));
            }
        }

        // **NEW**: Utility methods for section management

        // Check if any sections need remeshing
        inline bool HasDirtySections() const {
            return dirtyMeshSections.load(std::memory_order_relaxed) != 0;
        }

        // Get count of dirty sections
        inline int GetDirtySectionCount() const {
            uint32_t dirtyMask = dirtyMeshSections.load(std::memory_order_relaxed);
            return __builtin_popcountl(dirtyMask); // Count set bits
        }

        // Get count of sections with meshes
        inline int GetMeshSectionCount() const {
            return meshSectionCount.load(std::memory_order_relaxed);
        }

        // Check if chunk has any sections with data
        inline bool HasAnySections() const {
            for (const auto& section : sections) {
                if (section) {
                    return true;
                }
            }
            return false;
        }

        // Get all section indices that have data
        inline std::unordered_set<int> GetActiveSections() const {
            std::unordered_set<int> activeSections;
            for (int i = 0; i < Math::SECTIONS_PER_CHUNK; ++i) {
                if (sections[i]) {
                    activeSections.insert(i);
                }
            }
            return activeSections;
        }

        // **NEW**: Block modification with neighbor awareness

        // Set block and mark neighboring sections dirty if needed
        inline void SetBlockWithNeighbors(int localX, int worldY, int localZ, BlockID id) {
            SetBlock(localX, worldY, localZ, id);

            // Mark neighboring sections dirty if block is on section boundary
            int sectionIdx = SectionIndexFromGlobalY(worldY);
            int yInSection = LocalYFromGlobalY(worldY);

            // If block is on top boundary of section, mark section above dirty
            if (yInSection == Math::SECTION_HEIGHT - 1 && sectionIdx < Math::SECTIONS_PER_CHUNK - 1) {
                MarkSectionDirty(sectionIdx + 1);
            }

            // If block is on bottom boundary of section, mark section below dirty
            if (yInSection == 0 && sectionIdx > 0) {
                MarkSectionDirty(sectionIdx - 1);
            }
        }

        // **NEW**: Legacy compatibility methods

        // Update legacy flags based on section state
        inline void UpdateLegacyFlags() {
            bool anyDirty = HasDirtySections();
            bool anyMesh = GetMeshSectionCount() > 0;

            needsMesh.store(anyDirty, std::memory_order_relaxed);
            hasMesh.store(anyMesh, std::memory_order_relaxed);
            hasAnyMesh.store(anyMesh, std::memory_order_relaxed);
        }

        // Force remesh of entire chunk (marks all sections dirty)
        inline void ForceFullRemesh() {
            // Mark all possible sections dirty
            dirtyMeshSections.store(0xFFFFFFFF, std::memory_order_relaxed);
            needsMesh.store(true, std::memory_order_relaxed);

            Log::Debug("Forced full remesh for chunk (%d, %d)", pos.x, pos.z);
        }

        // Clear all mesh state (useful for unloading)
        inline void ClearAllMeshState() {
            dirtyMeshSections.store(0, std::memory_order_relaxed);
            meshSectionCount.store(0, std::memory_order_relaxed);
            hasAnyMesh.store(false, std::memory_order_relaxed);
            needsMesh.store(false, std::memory_order_relaxed);
            hasMesh.store(false, std::memory_order_relaxed);

            Log::Debug("Cleared all mesh state for chunk (%d, %d)", pos.x, pos.z);
        }

        // **NEW**: Performance and debugging utilities

        // Get chunk statistics
        struct ChunkStats {
            int totalSections = 0;
            int activeSections = 0;
            int dirtySections = 0;
            int meshedSections = 0;
            int totalBlocks = 0;
            int nonAirBlocks = 0;
        };

        ChunkStats GetStats() const {
            ChunkStats stats;
            stats.totalSections = Math::SECTIONS_PER_CHUNK;
            stats.dirtySections = GetDirtySectionCount();
            stats.meshedSections = GetMeshSectionCount();

            for (int i = 0; i < Math::SECTIONS_PER_CHUNK; ++i) {
                if (sections[i]) {
                    stats.activeSections++;

                    // Count blocks in this section
                    for (int x = 0; x < ChunkSection::SIZE; ++x) {
                        for (int y = 0; y < ChunkSection::SIZE; ++y) {
                            for (int z = 0; z < ChunkSection::SIZE; ++z) {
                                stats.totalBlocks++;
                                if (sections[i]->GetBlockID(x, y, z) != BlockID::Air) {
                                    stats.nonAirBlocks++;
                                }
                            }
                        }
                    }
                }
            }

            return stats;
        }

        // Print debug information about chunk state
        void PrintDebugInfo() const {
            auto stats = GetStats();
            Log::Info("Chunk (%d, %d) Debug Info:", pos.x, pos.z);
            Log::Info("  Sections: %d total, %d active, %d dirty, %d meshed",
                     stats.totalSections, stats.activeSections, stats.dirtySections, stats.meshedSections);
            Log::Info("  Blocks: %d total, %d non-air (%.1f%% filled)",
                     stats.totalBlocks, stats.nonAirBlocks,
                     stats.totalBlocks > 0 ? (100.0f * stats.nonAirBlocks / stats.totalBlocks) : 0.0f);
            Log::Info("  Legacy flags: needsMesh=%s, hasMesh=%s",
                     needsMesh.load() ? "true" : "false",
                     hasMesh.load() ? "true" : "false");
        }
    };

} // namespace Game