// File: src/common/world/level/ILevelWrite.hpp
//
// Read+write block access for code that must run on BOTH sides.
//
// Item behaviours (hoe tilling, shovel path, axe stripping, flint & steel,
// bone meal, buckets) are already common code, but they used to take a
// concrete Game::World* — which only the server has. That forced a remote
// client to wait a full round trip before a tilled block appeared. MC has no
// such split: ItemStack.useOn runs client-side inside
// MultiPlayerGameMode.startPrediction (MultiPlayerGameMode.java:347) against
// the client's own level, and the block edit it performs is captured by the
// prediction handler.
//
// Implementations:
//   Game::World          — server authority (writes the real world).
//   Client::ClientBlockAccess — client prediction (writes the client chunk
//                          cache through ClientChunkManager's prediction
//                          handler, so the ack can roll it back).
#pragma once

#include "../chunk/IBlockAccess.hpp"

namespace Game {

    class ILevelWrite : public IBlockAccess {
    public:
        // Mirrors World::SetBlock's flagged overload. `updateFlags` uses
        // Game::World::UpdateFlags values; the client implementation ignores
        // everything except the fact that a write happened (its remesh and
        // neighbour dirtying are handled by ClientChunkManager).
        virtual bool SetBlock(int worldX, int worldY, int worldZ,
                              BlockID blockId, uint32_t updateFlags) = 0;
    };

} // namespace Game
