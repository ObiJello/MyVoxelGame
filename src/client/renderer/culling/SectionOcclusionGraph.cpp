// File: src/client/renderer/culling/SectionOcclusionGraph.cpp
#include "SectionOcclusionGraph.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/SectionMesh.hpp"
#include "../mesh/ClientMeshManager.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Config.hpp"
#include <cmath>

namespace Render {

    void SectionOcclusionGraph::getNeighbor(Game::Math::ChunkPos pos, int sectionY, int dir,
                                             Game::Math::ChunkPos& outPos, int& outSY) {
        outPos = pos;
        outSY = sectionY;
        switch (dir) {
            case Direction::Down:  outSY = sectionY - 1; break;
            case Direction::Up:    outSY = sectionY + 1; break;
            case Direction::North: outPos.z = pos.z - 1; break;
            case Direction::South: outPos.z = pos.z + 1; break;
            case Direction::West:  outPos.x = pos.x - 1; break;
            case Direction::East:  outPos.x = pos.x + 1; break;
        }
    }

    void SectionOcclusionGraph::update(const glm::vec3& cameraPos, const Frustum& frustum,
                                        bool smartCull, int renderDistance,
                                        std::vector<SectionRenderData>& outVisible) {
        PROFILE_ZONE;

        int playerChunkX = static_cast<int>(std::floor(cameraPos.x / 16.0f));
        int playerChunkZ = static_cast<int>(std::floor(cameraPos.z / 16.0f));
        int playerSectionY = static_cast<int>(std::floor((cameraPos.y - Config::MinY) / 16.0f));
        playerSectionY = std::clamp(playerSectionY, 0, Game::Math::SECTIONS_PER_CHUNK - 1);

        // Flat 3D array for visited set — eliminates all hash map overhead.
        // Indexed by (rx, rz, sy) where rx/rz are relative to player chunk.
        int diameter = 2 * renderDistance + 1;
        int sectionsY = Game::Math::SECTIONS_PER_CHUNK;  // 24
        int gridSize = diameter * diameter * sectionsY;
        int chunkGridSize = diameter * diameter;

        // Use persistent buffers to avoid per-frame allocation.
        // Generation counter replaces memset — O(1) reset instead of zeroing the whole grid.
        m_gridNodes.resize(gridSize);
        m_currentGeneration++;
        // Handle wraparound (extremely unlikely but correct)
        if (m_currentGeneration == 0) {
            m_currentGeneration = 1;
            for (auto& node : m_gridNodes) node.generation = 0;
        }

        // Lazy chunk loaded cache — only query chunks we actually visit in BFS
        m_chunkLoaded.resize(chunkGridSize);

        auto getIdx = [&](int rx, int rz, int sy) -> int {
            return sy * chunkGridSize + rz * diameter + rx;
        };

        // Lazy chunk loaded lookup with caching
        auto isChunkLoaded = [&](int rx, int rz) -> bool {
            int ci = rz * diameter + rx;
            auto& entry = m_chunkLoaded[ci];
            if (entry.generation == m_currentGeneration) return entry.loaded;
            int cx = playerChunkX - renderDistance + rx;
            int cz = playerChunkZ - renderDistance + rz;
            entry.loaded = Client::g_clientChunkManager &&
                           Client::g_clientChunkManager->IsChunkLoaded({cx, cz});
            entry.generation = m_currentGeneration;
            return entry.loaded;
        };

        // Double-buffered BFS queues — cache-friendly alternative to std::queue/deque
        m_bfsCurrentQueue.clear();
        m_bfsNextQueue.clear();

        // Seed with player's section
        int startRX = renderDistance;  // Player is at center of grid
        int startRZ = renderDistance;
        int startIdx = getIdx(startRX, startRZ, playerSectionY);
        m_gridNodes[startIdx].generation = m_currentGeneration;
        m_gridNodes[startIdx].sourceDirections = 0x3F;  // All directions
        m_gridNodes[startIdx].gpuData = g_clientMeshManager ?
            g_clientMeshManager->GetGPUSectionData({playerChunkX, playerChunkZ}, playerSectionY) : nullptr;

        m_bfsCurrentQueue.push_back({static_cast<int16_t>(startRX), static_cast<int16_t>(startRZ),
                    static_cast<int8_t>(playerSectionY), 0x3F});

        int visitedCount = 0;
        int occludedCount = 0;

        // Direction offsets: dx, dz, dsy for each of 6 directions
        static constexpr int DIR_DX[] = {0, 0, 0, 0, -1, 1};
        static constexpr int DIR_DZ[] = {0, 0, -1, 1, 0, 0};
        static constexpr int DIR_DSY[] = {-1, 1, 0, 0, 0, 0};

        while (!m_bfsCurrentQueue.empty()) {
          for (const auto& entry : m_bfsCurrentQueue) {
            visitedCount++;

            int idx = getIdx(entry.rx, entry.rz, entry.sy);
            GridNode& node = m_gridNodes[idx];

            // World position for frustum test
            int worldCX = playerChunkX - renderDistance + entry.rx;
            int worldCZ = playerChunkZ - renderDistance + entry.rz;
            float sectionMinX = static_cast<float>(worldCX * 16);
            float sectionMinY = static_cast<float>(entry.sy * 16 + Config::MinY);
            float sectionMinZ = static_cast<float>(worldCZ * 16);

            bool inFrustum = frustum.IsBoxVisible(
                glm::vec3(sectionMinX, sectionMinY, sectionMinZ),
                glm::vec3(sectionMinX + 16.0f, sectionMinY + 16.0f, sectionMinZ + 16.0f));

            // Add to visible list if in frustum AND has geometry
            if (inFrustum && node.gpuData && node.gpuData->HasGeometry()) {
                float dx = (sectionMinX + 8.0f) - cameraPos.x;
                float dy = (sectionMinY + 8.0f) - cameraPos.y;
                float dz = (sectionMinZ + 8.0f) - cameraPos.z;
                float dist = (dx * dx + dz * dz) + (dy * dy * 0.01f);

                Game::Math::ChunkPos chunkPos{worldCX, worldCZ};
                SectionRenderData rd(chunkPos, entry.sy, node.gpuData, dist);
                rd.inFrustum = true;
                rd.layerMask = 0;
                if (node.gpuData->opaqueVertexCount > 0)      rd.layerMask |= 1;
                if (node.gpuData->cutoutVertexCount > 0)       rd.layerMask |= 2;
                if (node.gpuData->translucentVertexCount > 0)  rd.layerMask |= 4;
                outVisible.push_back(rd);
            }

            // Determine VisibilitySet:
            //   Starting section: always all-visible (player can see in all directions)
            //   COMPILED (has gpuData): use real VisibilitySet from mesh build
            //   AIR (isAllAir): chunk loaded, section confirmed all air — all faces visible
            //   SOLID (not isAllAir, no gpuData): unmeshed solid section — blocks BFS
            VisibilitySet currentVis;  // Default: all-zeros (opaque/blocks BFS)
            bool isStartingSection = (entry.rx == startRX && entry.rz == startRZ &&
                                      entry.sy == playerSectionY);
            if (isStartingSection) {
                currentVis.setAll(true);  // Player's section — always see in all directions
            } else if (node.gpuData) {
                currentVis = node.gpuData->visibilitySet;
            } else if (Client::g_clientChunkManager) {
                auto* secInfo = Client::g_clientChunkManager->GetSectionInfo(
                    {worldCX, worldCZ}, entry.sy);
                if (secInfo && secInfo->isAllAir) {
                    currentVis.setAll(true);  // Confirmed air — all faces visible
                }
                // else: has non-air blocks but no mesh yet — treat as opaque
            }

            // Distant LOS check flag — computed ONCE per current node (Minecraft-style).
            // Only applies when the CURRENT section is >3 sections from the player.
            bool distantFromCamera = smartCull && (
                std::abs(entry.rx - startRX) > 3 ||
                std::abs(entry.rz - startRZ) > 3 ||
                std::abs(entry.sy - playerSectionY) > 3);

            // Explore 6 neighbors
            for (int dir = 0; dir < 6; dir++) {
                int nrx = entry.rx + DIR_DX[dir];
                int nrz = entry.rz + DIR_DZ[dir];
                int nsy = entry.sy + DIR_DSY[dir];

                // Bounds check
                if (nrx < 0 || nrx >= diameter || nrz < 0 || nrz >= diameter)
                    continue;
                if (nsy < 0 || nsy >= sectionsY)
                    continue;

                int nIdx = getIdx(nrx, nrz, nsy);

                // Already visited? Just update source directions
                if (m_gridNodes[nIdx].generation == m_currentGeneration) {
                    m_gridNodes[nIdx].sourceDirections |= (1 << dir);
                    continue;
                }

                // Check if chunk is loaded (lazy cached lookup)
                if (!isChunkLoaded(nrx, nrz))
                    continue;

                // SMART CULL: check VisibilitySet
                if (smartCull) {
                    bool canReach = false;
                    uint8_t srcDirs = node.sourceDirections;
                    for (int srcBit = 0; srcBit < 6; srcBit++) {
                        if (!(srcDirs & (1 << srcBit))) continue;
                        if (currentVis.canSeeThrough(Direction::opposite(srcBit), dir)) {
                            canReach = true;
                            break;
                        }
                    }
                    if (!canReach) {
                        occludedCount++;
                        continue;
                    }
                }

                // DISTANT LOS CHECK (Minecraft-style): raycast from the CURRENT
                // section's corner toward the camera. If the ray passes through any
                // unvisited section, reject this neighbor.
                // Key differences from our old code (now matching Minecraft):
                //   1. distantFromCamera is for the CURRENT node, not neighbor
                //   2. Ray origin is the CURRENT section, not the neighbor
                //   3. Corner selection uses Minecraft's axis-aware inversion
                if (distantFromCamera) {
                    // Ray origin: CURRENT section's block-space origin
                    float originX = sectionMinX;
                    float originY = sectionMinY;
                    float originZ = sectionMinZ;

                    // Pick corner: for the axis matching the BFS direction, invert
                    // the comparison (Minecraft lines 288-290)
                    bool isXAxis = (dir == Direction::West || dir == Direction::East);
                    bool isYAxis = (dir == Direction::Down || dir == Direction::Up);
                    bool isZAxis = (dir == Direction::North || dir == Direction::South);
                    bool mX = isXAxis ? (cameraPos.x > originX) : (cameraPos.x < originX);
                    bool mY = isYAxis ? (cameraPos.y > originY) : (cameraPos.y < originY);
                    bool mZ = isZAxis ? (cameraPos.z > originZ) : (cameraPos.z < originZ);

                    float ckX = originX + (mX ? 16.0f : 0.0f);
                    float ckY = originY + (mY ? 16.0f : 0.0f);
                    float ckZ = originZ + (mZ ? 16.0f : 0.0f);

                    float rdx = cameraPos.x - ckX;
                    float rdy = cameraPos.y - ckY;
                    float rdz = cameraPos.z - ckZ;
                    float rlen = std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz);

                    if (rlen > 0.001f) {
                        float rscale = 28.0f / rlen;
                        float sX = rdx * rscale, sY = rdy * rscale, sZ = rdz * rscale;
                        bool losVisible = true;

                        while ((ckX - cameraPos.x) * (ckX - cameraPos.x) +
                               (ckY - cameraPos.y) * (ckY - cameraPos.y) +
                               (ckZ - cameraPos.z) * (ckZ - cameraPos.z) > 3600.0f) {
                            ckX += sX; ckY += sY; ckZ += sZ;

                            if (ckY > static_cast<float>(Config::MaxY) ||
                                ckY < static_cast<float>(Config::MinY)) break;

                            int rcx = static_cast<int>(std::floor(ckX / 16.0f));
                            int rsy = static_cast<int>(std::floor((ckY - Config::MinY) / 16.0f));
                            int rcz = static_cast<int>(std::floor(ckZ / 16.0f));
                            rsy = std::clamp(rsy, 0, sectionsY - 1);
                            int rrx = (rcx - playerChunkX) + renderDistance;
                            int rrz = (rcz - playerChunkZ) + renderDistance;

                            if (rrx < 0 || rrx >= diameter || rrz < 0 || rrz >= diameter) {
                                losVisible = false;
                                break;
                            }
                            if (m_gridNodes[getIdx(rrx, rrz, rsy)].generation != m_currentGeneration) {
                                losVisible = false;
                                break;
                            }
                        }
                        if (!losVisible) {
                            occludedCount++;
                            continue;
                        }
                    }
                }

                // Mark visited and look up GPU data
                m_gridNodes[nIdx].generation = m_currentGeneration;
                m_gridNodes[nIdx].sourceDirections = (1 << dir);
                int ncx = playerChunkX - renderDistance + nrx;
                int ncz = playerChunkZ - renderDistance + nrz;
                m_gridNodes[nIdx].gpuData = g_clientMeshManager ?
                    g_clientMeshManager->GetGPUSectionData({ncx, ncz}, nsy) : nullptr;

                m_bfsNextQueue.push_back({static_cast<int16_t>(nrx), static_cast<int16_t>(nrz),
                            static_cast<int8_t>(nsy),
                            static_cast<uint8_t>(1 << dir)});
            }
          }
          m_bfsCurrentQueue.clear();
          std::swap(m_bfsCurrentQueue, m_bfsNextQueue);
        }

        m_lastVisitedCount = visitedCount;
        m_lastOccludedCount = occludedCount;
        m_lastRenderedCount = static_cast<int>(outVisible.size());
        m_needsFullUpdate = false;
    }

} // namespace Render
