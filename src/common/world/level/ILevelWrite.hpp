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

        // Same, carrying the block-state index (MC BlockState.getId()). Needed
        // by any behaviour that edits a PROPERTY rather than swapping the
        // block — a shovel dousing a campfire writes the same block back with
        // `lit=false`, so the two-argument form above would reset it to the
        // default state and relight it.
        //
        // Both implementations already had this overload; it is declared here
        // so common code can reach it through the interface.
        virtual bool SetBlock(int worldX, int worldY, int worldZ,
                              BlockID blockId, uint32_t updateFlags,
                              BlockStateIndex stateIndex) = 0;

        // MC's `Level.setBlock(pos, state, flags)` — the form callers should
        // use. Non-virtual on purpose: it forwards to the pair overload above,
        // which is what implementors override and what the storage layer still
        // speaks. Argument order follows vanilla (state, then flags), NOT the
        // pair form's (flags, then index).
        bool SetBlock(int worldX, int worldY, int worldZ,
                      BlockState state, uint32_t updateFlags) {
            return SetBlock(worldX, worldY, worldZ, state.Block(), updateFlags, state.Index());
        }

        // MC Level.isClientSide. Item behaviours run on both sides (see the
        // note above), and most of them WANT that — a tilled block should
        // appear immediately and be rolled back if the server disagrees.
        //
        // Some do not. Anything that spawns an entity has no client-side
        // equivalent to predict: the entity only exists once the server sends
        // it, so running the spawn during prediction would either do nothing
        // or, in single-player where both sides share a process, spawn twice.
        // MC's own SpawnEggItem opens with `if (!(level instanceof ServerLevel))
        // return SUCCESS;` for exactly this reason, and this is the flag that
        // branch needs.
        virtual bool IsClientSide() const = 0;
    };

} // namespace Game
