#include "WorldManager.hpp"
#include "ChunkProvider.hpp"
#include "../render/ChunkRenderer.hpp"  // for g_chunkMeshes
#include "Mesher.hpp"

namespace Game {

    std::unordered_set<ChunkPos,ChunkPosHash> WorldManager::s_loaded;

    void WorldManager::Update(const glm::vec3 &cameraPos) {
        // 1) figure out camera chunk
        int cx = int(std::floor(cameraPos.x / Math::CHUNK_SIZE));
        int cz = int(std::floor(cameraPos.z / Math::CHUNK_SIZE));
        ChunkPos cam{cx,cz};

        // 2) build desired set
        std::unordered_set<ChunkPos,ChunkPosHash> want;
        for(int dz=-RENDER_RADIUS; dz<=RENDER_RADIUS; ++dz)
            for(int dx=-RENDER_RADIUS; dx<=RENDER_RADIUS; ++dx)
                want.insert({cx+dx, cz+dz});

        // 3) load any new
        for(auto &pos : want) {
            if (!s_loaded.count(pos)) {
                LoadChunk(pos);
                s_loaded.insert(pos);
            }
        }

        // 4) unload any old
        for(auto it = s_loaded.begin(); it != s_loaded.end(); ) {
            if (!want.count(*it)) {
                UnloadChunk(*it);
                it = s_loaded.erase(it);
            } else {
                ++it;
            }
        }
    }

    void WorldManager::LoadChunk(ChunkPos p) {
        ChunkProvider::RequestChunk(p);
    }

    void WorldManager::UnloadChunk(ChunkPos p) {
        // 1) toss GPU meshes
        auto &meshes = Render::g_chunkMeshes;
        meshes.erase(std::remove_if(meshes.begin(), meshes.end(),
            [&](auto &cm){ return cm.chunkXZ == p; }),
            meshes.end());

        // 2) drop from ChunkProvider cache
        ChunkProvider::UnloadChunk(p);
    }

} // namespace Game