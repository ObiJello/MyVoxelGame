// File: src/client/renderer/mesh/RenderRegionCache.hpp
//
// Port of net.minecraft.client.renderer.chunk.RenderRegionCache.
//
// Builds the 3x3x3 RegionSnapshot a mesh job is compiled against, memoising the
// per-section copies so neighbouring jobs share them:
//
//     private SectionCopy getSectionDataCopy(level, x, y, z) {
//        return this.sectionCopyCache.computeIfAbsent(SectionPos.asLong(x,y,z),
//               k -> new SectionCopy(level.getChunk(x, z), ...));
//     }
//
// Scope is ONE scheduling pass, exactly as MC constructs `new RenderRegionCache()`
// at the top of every LevelRenderer.compileSections call. Per-pass rather than
// persistent because the copies must not outlive the frame they were taken in —
// a section edited afterwards would otherwise be meshed from stale blocks.
//
// The sharing is what makes an unbounded compile queue affordable. Meshing a
// section reads it and its 26 neighbours, so a private copy per job stores the
// same blocks up to 27 times; with the cache, N clustered jobs cost roughly N
// section copies plus a one-section halo.
#pragma once

#include "MeshJobData.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include <cstdint>
#include <unordered_map>

namespace Client {

    class ClientChunkManager;

    namespace Render {

        class RenderRegionCache {
        public:
            // Build the region centred on (chunkPos, sectionY). Sections outside
            // the world, or in chunks that are not loaded, come back null and
            // read as air — MC's EmptyLevelChunk case.
            //
            // MAIN THREAD ONLY: reads the live chunk map.
            RegionSnapshot CreateRegion(ClientChunkManager& chunks,
                                        Game::Math::ChunkPos chunkPos, int sectionY);

            void Clear() { m_sections.clear(); }
            size_t DistinctSections() const { return m_sections.size(); }

        private:
            // Key is the section position packed into 64 bits: 26 bits of X, 26
            // of Z, 12 of Y — the same shape as MC's SectionPos.asLong.
            static uint64_t Key(int sx, int sy, int sz) {
                return (static_cast<uint64_t>(static_cast<uint32_t>(sx) & 0x3FFFFFFu) << 38)
                     | (static_cast<uint64_t>(static_cast<uint32_t>(sz) & 0x3FFFFFFu) << 12)
                     |  (static_cast<uint64_t>(static_cast<uint32_t>(sy) & 0xFFFu));
            }

            SectionCopyPtr GetOrCreate(ClientChunkManager& chunks,
                                       int sectionX, int sectionY, int sectionZ);

            std::unordered_map<uint64_t, SectionCopyPtr> m_sections;
        };

    } // namespace Render
} // namespace Client
