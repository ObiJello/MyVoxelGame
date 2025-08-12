// File: src/common/world/chunk/IBlockAccess.hpp
#pragma once

#include "../block/Blocks.hpp"

namespace Game {

    // Minimal interface for read-only block access
    // This allows physics, meshing, and other systems to access block data
    // without depending on the entire World class
    struct IBlockAccess {
        virtual ~IBlockAccess() = default;

        // Core block access methods
        virtual BlockID GetBlock(int worldX, int worldY, int worldZ) const = 0;

        // Chunk loading state queries
        virtual bool IsChunkLoaded(int chunkX, int chunkZ) const = 0;
        virtual bool IsPositionLoaded(int worldX, int worldY, int worldZ) const = 0;

        // Convenience methods for physics and other systems
        virtual bool IsBlockSolid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsBlockFluid(int worldX, int worldY, int worldZ) const = 0;
        virtual bool IsValidPosition(int worldX, int worldY, int worldZ) const = 0;
    };

} // namespace Game