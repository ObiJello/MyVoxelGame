// File: src/server/entity/MorphBlockAnchor.hpp
//
// /morph block, locked to the grid (Left Alt): the cell the player locked
// into gets the REAL block. Every other player then walks on it, mines it,
// opens it (a chest morph is a chest) exactly as on any block — nothing is
// approximated, the world holds the block. The morphed player goes
// invisible so their model and the block do not draw twice; their client
// keeps them pinned in the cell. Releasing the lock (Alt again, morphing
// into something else, leaving) takes the block away again. If someone
// mines the block, the lock is over: the server forgets it and the client,
// seeing the cell empty, releases itself. Server thread only.
#pragma once

#include "common/world/block/Blocks.hpp"
#include "common/world/level/DimensionId.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Server {

    class PlayerSessionManager;

    class MorphBlockAnchor {
    public:
        explicit MorphBlockAnchor(PlayerSessionManager& sessions) : m_sessions(sessions) {}

        // Place the player's morph block in `cell`. False when it could not
        // be (not a block morph, cell not loaded, cell not empty).
        bool Lock(uint32_t playerId, const glm::ivec3& cell);
        void Unlock(uint32_t playerId);
        // The morph turned (Shift+Alt) while locked: the placed block takes
        // the turned state (a ladder moves to the next wall).
        void Refresh(uint32_t playerId);
        void Tick();
        bool IsLocked(uint32_t playerId) const;

    private:
        struct Entry {
            uint32_t          player = 0;
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            glm::ivec3        cell{0};
            Game::BlockID     block = Game::BlockID::Air;
        };
        void Remove(const Entry& e, bool takeBlock);

        PlayerSessionManager& m_sessions;
        std::vector<Entry>    m_entries;
    };

} // namespace Server
