// File: src/common/world/lighting/ChunkSkyLightSources.cpp
#include "common/world/lighting/ChunkSkyLightSources.hpp"

#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/LightStateAccess.hpp"
#include "common/world/chunk/Chunk.hpp"

namespace Game::Lighting {

    namespace {
        inline bool EdgeOccluded(uint32_t top, uint32_t bottom) {
            return BlockLightProperties::IsEdgeOccluded(BlockState::FromRawId(top), BlockState::FromRawId(bottom));
        }
    }

    void ChunkSkyLightSources::FillFrom(const Chunk& chunk) {
        const int maxSectionIndex = chunk.HighestFilledSectionIndex();
        if (maxSectionIndex == Chunk::kNoFilledSection) {
            m_heights.fill(0);                               // every column at kMinY
            return;
        }
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const int edgeY = FindLowestSourceY(chunk, maxSectionIndex, x, z);
                Set(Index(x, z), edgeY > kMinY ? edgeY : kMinY);
            }
        }
    }

    // MC findLowestSourceY — walks the column from just above the top filled
    // section down to the first occluded edge. All-air sections are skipped
    // whole (their top state resets to air, as in MC).
    int ChunkSkyLightSources::FindLowestSourceY(const Chunk& chunk, int topSectionIndex, int x, int z) const {
        int topY = Config::MinY + (topSectionIndex + 1) * 16;
        uint32_t topState = 0;                               // air
        for (int sectionIndex = topSectionIndex; sectionIndex >= 0; --sectionIndex) {
            const ChunkSection* section = chunk.GetSection(sectionIndex);
            if (section->IsAllAir()) {
                topState = 0;
                topY = Config::MinY + sectionIndex * 16;
                continue;
            }
            const PalettedContainer& states = section->States();
            for (int y = 15; y >= 0; --y) {
                const uint32_t bottomState = states.Get(static_cast<size_t>((y << 8) | (z << 4) | x));
                if (EdgeOccluded(topState, bottomState)) return topY;
                topState = bottomState;
                --topY;
            }
        }
        return kMinY;
    }

    bool ChunkSkyLightSources::Update(const Chunk& chunk, int x, int y, int z) {
        const int upperEdgeY = y + 1;
        const int index = Index(x, z);
        const int currentLowestSourceY = Get(index);
        if (upperEdgeY < currentLowestSourceY) return false;
        const uint32_t topState = StateIdAt(chunk, x, y + 1, z);
        const uint32_t middleState = StateIdAt(chunk, x, y, z);
        if (UpdateEdge(chunk, index, currentLowestSourceY, x, y + 1, topState, y, middleState, z)) {
            return true;
        }
        const uint32_t bottomState = StateIdAt(chunk, x, y - 1, z);
        return UpdateEdge(chunk, index, currentLowestSourceY, x, y, middleState, y - 1, bottomState, z);
    }

    bool ChunkSkyLightSources::UpdateEdge(const Chunk& chunk, int index, int oldTopEdgeY,
                                          int x, int topY, uint32_t topState,
                                          int bottomY, uint32_t bottomState, int z) {
        const int checkedEdgeY = topY;
        if (EdgeOccluded(topState, bottomState)) {
            if (checkedEdgeY > oldTopEdgeY) {
                Set(index, checkedEdgeY);
                return true;
            }
        } else if (checkedEdgeY == oldTopEdgeY) {
            Set(index, FindLowestSourceBelow(chunk, x, bottomY, bottomState, z));
            return true;
        }
        return false;
    }

    int ChunkSkyLightSources::FindLowestSourceBelow(const Chunk& chunk, int x, int startY,
                                                    uint32_t startState, int z) const {
        int topY = startY;
        uint32_t topState = startState;
        for (int bottomY = startY - 1; bottomY >= kMinY; --bottomY) {
            const uint32_t bottomState = StateIdAt(chunk, x, bottomY, z);
            if (EdgeOccluded(topState, bottomState)) return topY;
            topState = bottomState;
            topY = bottomY;
        }
        return kMinY;
    }

} // namespace Game::Lighting
