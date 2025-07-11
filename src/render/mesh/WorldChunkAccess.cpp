// File: src/render/mesh/WorldChunkAccess.cpp

#include "Mesher.hpp"
#include "../../engine/world/World.hpp"
#include "../../core/Log.hpp"

namespace Render {

    WorldChunkAccess::WorldChunkAccess(const Game::World* world) : m_world(world) {
        if (!m_world) {
            Log::Warning("WorldChunkAccess created with null world pointer");
        }
    }

    Game::BlockID WorldChunkAccess::GetBlockAt(int worldX, int worldY, int worldZ) const {
        if (!m_world) {
            return Game::BlockID::Air;
        }

        // Use World's block access method which handles chunk loading/bounds checking
        return m_world->GetBlock(worldX, worldY, worldZ);
    }

    bool WorldChunkAccess::IsChunkLoaded(int chunkX, int chunkZ) const {
        if (!m_world) {
            return false;
        }

        return m_world->IsChunkLoaded(chunkX, chunkZ);
    }

} // namespace Render