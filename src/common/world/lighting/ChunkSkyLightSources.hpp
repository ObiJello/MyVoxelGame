// File: src/common/world/lighting/ChunkSkyLightSources.hpp
//
// MC `net.minecraft.world.level.lighting.ChunkSkyLightSources`: per column of
// one chunk, the LOWEST Y whose cell still receives full (15) sky light
// straight from above — the top of the first "edge" sky light cannot cross
// (ChunkSkyLightSources.isEdgeOccluded: any light-dampening block, or two
// face shapes that close the boundary). Every cell at or above it in the
// column is a sky SOURCE; the sky engine only has to flood outward from the
// source boundary.
//
// Kept on the chunk (Chunk::light.skySources), filled when the chunk is first
// lit and kept current by World::SetBlock through Update — exactly
// LevelChunk.setBlockState's `skyLightSources.update(this, x, y, z)`.
#pragma once

#include "common/core/Config.hpp"

#include <array>
#include <climits>
#include <cstdint>

namespace Game {
    class Chunk;
}

namespace Game::Lighting {

    class ChunkSkyLightSources {
    public:
        // MC: minY = level.getMinY() - 1 — one below the world, where a
        // column with nothing in it bottoms out.
        static constexpr int kMinY = Config::MinY - 1;
        static constexpr int kNegativeInfinity = INT_MIN;

        ChunkSkyLightSources() { m_heights.fill(0); }   // every column at kMinY

        // MC fillFrom: scan every column from the top filled section down.
        void FillFrom(const Chunk& chunk);

        // MC update(level, x, y, z): the block at chunk-local (x, worldY, z)
        // changed. Returns true when the column's source boundary moved.
        bool Update(const Chunk& chunk, int localX, int worldY, int localZ);

        // MC getLowestSourceY: kNegativeInfinity for a column open to the
        // bottom of the world (extendSourcesBelowWorld).
        int GetLowestSourceY(int localX, int localZ) const {
            const int v = Get(Index(localX, localZ));
            return v == kMinY ? kNegativeInfinity : v;
        }

        // MC getHighestLowestSourceY.
        int GetHighestLowestSourceY() const {
            int maxValue = INT_MIN;
            for (int16_t h : m_heights) maxValue = h > maxValue ? h : maxValue;
            const int v = maxValue + kMinY;
            return v == kMinY ? kNegativeInfinity : v;
        }

    private:
        static int Index(int x, int z) { return x + z * 16; }
        int  Get(int index) const { return m_heights[static_cast<size_t>(index)] + kMinY; }
        void Set(int index, int value) { m_heights[static_cast<size_t>(index)] = static_cast<int16_t>(value - kMinY); }

        int  FindLowestSourceY(const Chunk& chunk, int topSectionIndex, int x, int z) const;
        bool UpdateEdge(const Chunk& chunk, int index, int oldTopEdgeY,
                        int x, int topY, uint32_t topState, int bottomY, uint32_t bottomState, int z);
        int  FindLowestSourceBelow(const Chunk& chunk, int x, int startY, uint32_t startState, int z) const;

        std::array<int16_t, 256> m_heights;
    };

} // namespace Game::Lighting
