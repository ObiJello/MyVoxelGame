// File: src/common/world/chunk/IBlockAccess.hpp
#pragma once

#include "../block/Blocks.hpp"
#include "common/world/biome/Biomes.hpp"

namespace Game {

    // Minimal interface for read-only block access
    // This allows physics, meshing, and other systems to access block data
    // without depending on the entire World class
    struct IBlockAccess {
        virtual ~IBlockAccess() = default;

        // Core block access methods
        virtual BlockID GetBlock(int worldX, int worldY, int worldZ) const = 0;

        // State index within the block's own state list (MC BlockState.getId());
        // 0 is always the block's default state. NOT pure virtual on purpose:
        // "I don't track state" and "everything is at its default state" are the
        // same answer, so accessors that only carry block ids (mesher snapshots,
        // physics views) stay correct without implementing anything.
        virtual uint8_t GetBlockState(int /*worldX*/, int /*worldY*/, int /*worldZ*/) const { return 0; }

        // Biome id at a world position, quantised to MC's 4x4x4 noise-biome
        // grid by the chunk. Accessors with no biome data answer 0, which is
        // the fallback biome — so a colour query degrades to plains rather
        // than failing, exactly as it did before biomes existed.
        virtual uint16_t GetBiome(int /*worldX*/, int /*worldY*/, int /*worldZ*/) const {
            return kFallbackBiomeId;
        }

        // Chunk loading state queries
        virtual bool IsChunkLoaded(int chunkX, int chunkZ) const = 0;
        virtual bool IsPositionLoaded(int worldX, int worldY, int worldZ) const = 0;

        // Convenience methods for physics and other systems
        virtual bool IsBlockSolid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsBlockFluid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsValidPosition(int worldX, int worldY, int worldZ) const = 0;
    };

} // namespace Game