// File: src/client/renderer/mesh/RenderRegionCache.cpp
#include "RenderRegionCache.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include <cstring>

namespace Client {
namespace Render {

    SectionCopyPtr RenderRegionCache::GetOrCreate(ClientChunkManager& chunks,
                                                  int sectionX, int sectionY, int sectionZ) {
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return nullptr;   // outside build height — reads as air
        }

        const uint64_t key = Key(sectionX, sectionY, sectionZ);
        auto it = m_sections.find(key);
        if (it != m_sections.end()) {
            return it->second;   // shared with every other job that touches it
        }

        const Game::Math::ChunkPos chunkPos{sectionX, sectionZ};
        ClientChunk* chunk = chunks.GetChunk(chunkPos);
        if (!chunk || chunk->state != ChunkState::LOADED || !chunk->chunkData) {
            // MC's EmptyLevelChunk path: cache the null so the other jobs in
            // this pass do not re-look-up a chunk that is not there.
            m_sections.emplace(key, nullptr);
            return nullptr;
        }

        auto copy = std::make_shared<SectionCopy>();
        const Game::ChunkSection* section = chunk->chunkData->GetSection(sectionY);

        // MC: `this.section = levelChunkSection.hasOnlyAir() ? null : states.copy()`.
        // An all-air section stores nothing at all, which is most of a 384-block
        // column — the single biggest reason this is affordable.
        // MC: `this.section = levelChunkSection.hasOnlyAir() ? null : states.copy()`.
        // A uniform section's container carries a one-entry palette and no
        // backing words, so the "all air costs nothing" property comes for free.
        if (section && !section->IsAllAir()) {
            copy->allAir = false;
            copy->states = section->States();     // palette + packed words
        }
        if (section) {
            copy->biomes = section->Biomes();
            copy->hasBiomes = true;
        }

        auto stored = SectionCopyPtr(std::move(copy));
        m_sections.emplace(key, stored);
        return stored;
    }

    RegionSnapshot RenderRegionCache::CreateRegion(ClientChunkManager& chunks,
                                                   Game::Math::ChunkPos chunkPos,
                                                   int sectionY) {
        PROFILE_ZONE_N("CreateRegion");

        RegionSnapshot region;
        region.minSectionX = chunkPos.x - RegionSnapshot::RADIUS;
        region.minSectionY = sectionY   - RegionSnapshot::RADIUS;
        region.minSectionZ = chunkPos.z - RegionSnapshot::RADIUS;

        // MC createRegion's triple loop over the 3x3x3 neighbourhood.
        for (int dz = 0; dz < RegionSnapshot::SIZE; ++dz) {
            for (int dy = 0; dy < RegionSnapshot::SIZE; ++dy) {
                for (int dx = 0; dx < RegionSnapshot::SIZE; ++dx) {
                    region.sections[RegionSnapshot::Index(dx, dy, dz)] =
                        GetOrCreate(chunks,
                                    region.minSectionX + dx,
                                    region.minSectionY + dy,
                                    region.minSectionZ + dz);
                }
            }
        }

        return region;
    }

} // namespace Render
} // namespace Client
