// File: src/client/world/ClientBlockAccess.hpp
#pragma once

#include "common/world/chunk/IBlockAccess.hpp"

namespace Client {

    // IBlockAccess implementation backed by the client chunk cache.
    // Used for physics and raycasting when connected to a remote server
    // (no server-side World available in the same process).
    class ClientBlockAccess : public Game::IBlockAccess {
    public:
        ClientBlockAccess() = default;
        ~ClientBlockAccess() override = default;

        // IBlockAccess interface
        Game::BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsValidPosition(int worldX, int worldY, int worldZ) const override;
    };

} // namespace Client
