// File: src/client/renderer/culling/SectionOcclusionGraph.cpp
#include "SectionOcclusionGraph.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/SectionMesh.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/Config.hpp"
#include <algorithm>
#include <cmath>

namespace Render {

    SectionOcclusionGraph::~SectionOcclusionGraph() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    // ========================================================================
    // INPUT SNAPSHOT (main thread)
    // ========================================================================

    void SectionOcclusionGraph::BuildInput(BfsJob& job, const glm::vec3& cameraPos,
                                           bool smartCull, int renderDistance) {
        PROFILE_ZONE_N("BfsSnapshot");

        job.cameraPos = cameraPos;
        job.smartCull = smartCull;
        job.renderDistance = renderDistance;
        job.playerChunkX = static_cast<int>(std::floor(cameraPos.x / 16.0f));
        job.playerChunkZ = static_cast<int>(std::floor(cameraPos.z / 16.0f));
        job.playerSectionY = std::clamp(
            static_cast<int>(std::floor((cameraPos.y - Config::MinY) / 16.0f)),
            0, Game::Math::SECTIONS_PER_CHUNK - 1);

        const int diameter = 2 * renderDistance + 1;
        const int sectionsY = Game::Math::SECTIONS_PER_CHUNK;
        const int chunkGridSize = diameter * diameter;
        job.diameter = diameter;

        job.cells.assign(static_cast<size_t>(chunkGridSize) * sectionsY, BfsJob::Cell{});
        job.chunkLoaded.assign(chunkGridSize, 0);
        job.result.clear();
        job.visitedCount = 0;
        job.occludedCount = 0;
        job.centerLoaded = false;

        if (!Client::g_clientChunkManager) return;

        for (int rz = 0; rz < diameter; ++rz) {
            for (int rx = 0; rx < diameter; ++rx) {
                const int cx = job.playerChunkX - renderDistance + rx;
                const int cz = job.playerChunkZ - renderDistance + rz;
                const Client::ClientChunk* chunk =
                    Client::g_clientChunkManager->GetChunk({cx, cz});
                if (!chunk || chunk->state != Client::ChunkState::LOADED) continue;

                const int ci = rz * diameter + rx;
                job.chunkLoaded[ci] = 1;

                for (int sy = 0; sy < sectionsY; ++sy) {
                    BfsJob::Cell& cell = job.cells[static_cast<size_t>(sy) * chunkGridSize + ci];
                    const auto& si = chunk->sectionInfos[sy];
                    GPUSectionData* gpu = si.gpuData.load(std::memory_order_acquire);
                    if (gpu && gpu->HasGeometry()) {
                        cell.gpuData = gpu;
                        cell.visBits = gpu->visibilitySet.raw();
                        cell.layerMask = 0;
                        if (gpu->opaqueVertexCount > 0)      cell.layerMask |= 1;
                        if (gpu->cutoutVertexCount > 0)      cell.layerMask |= 2;
                        if (gpu->translucentVertexCount > 0) cell.layerMask |= 4;
                    } else if (si.isAllAir) {
                        cell.visBits = VisibilitySet::kAllVisibleBits;
                    }
                    // else: non-air but unmeshed — visBits 0 blocks BFS (opaque)
                }
            }
        }

        job.centerLoaded =
            job.chunkLoaded[static_cast<size_t>(renderDistance) * diameter + renderDistance] != 0;
    }

    // ========================================================================
    // WORKER THREAD
    // ========================================================================

    bool SectionOcclusionGraph::Busy() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending != nullptr || m_running || m_completed != nullptr;
    }

    void SectionOcclusionGraph::SubmitAsync(std::unique_ptr<BfsJob> job) {
        EnsureWorkerStarted();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_inFlightActive = true;
            m_inFlightBuildCounter = job->buildCounter;
            m_pending = std::move(job);
        }
        m_cv.notify_one();
    }

    std::unique_ptr<SectionOcclusionGraph::BfsJob> SectionOcclusionGraph::TryCollect() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_completed) m_inFlightActive = false;
        return std::move(m_completed);
    }

    bool SectionOcclusionGraph::InFlightBuildCounter(uint32_t& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_inFlightActive) return false;
        out = m_inFlightBuildCounter;
        return true;
    }

    std::unique_ptr<SectionOcclusionGraph::BfsJob> SectionOcclusionGraph::AcquireJob() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pool) return std::move(m_pool);
        }
        return std::make_unique<BfsJob>();
    }

    void SectionOcclusionGraph::RecycleJob(std::unique_ptr<BfsJob> job) {
        if (!job) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool = std::move(job);  // keeps the big cell/result buffers allocated
    }

    void SectionOcclusionGraph::EnsureWorkerStarted() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_worker.joinable()) {
            m_worker = std::thread([this] { WorkerLoop(); });
        }
    }

    void SectionOcclusionGraph::WorkerLoop() {
        PROFILE_THREAD("OcclusionBFS");
        for (;;) {
            std::unique_ptr<BfsJob> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_shutdown || m_pending != nullptr; });
                if (m_shutdown) return;
                job = std::move(m_pending);
                m_running = true;
            }

            Run(*job, m_workerScratch);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_completed = std::move(job);
                m_running = false;
            }
        }
    }

    // ========================================================================
    // THE BFS (thread-agnostic — reads only the job snapshot + scratch)
    // ========================================================================

    void SectionOcclusionGraph::Run(BfsJob& job, Scratch& scratch) {
        PROFILE_ZONE_N("OcclusionBFS");

        const int diameter = job.diameter;
        const int sectionsY = Game::Math::SECTIONS_PER_CHUNK;
        const int chunkGridSize = diameter * diameter;
        const int gridSize = chunkGridSize * sectionsY;
        const glm::vec3 cameraPos = job.cameraPos;

        // Camera chunk absent from the snapshot (not streamed in yet): the
        // seed has no visibility data to spread through, so a normal run
        // would return an empty set and blank the frame. Degrade gracefully
        // instead — traverse unloaded columns and skip occlusion so every
        // meshed section in range is returned (the per-frame frustum filter
        // still applies). Occlusion resumes as soon as the chunk arrives.
        const bool blind = !job.centerLoaded;
        const bool smartCull = job.smartCull && !blind;

        // Generation-counter visited grid — O(1) reset
        scratch.gridNodes.resize(gridSize);
        scratch.generation++;
        if (scratch.generation == 0) {
            scratch.generation = 1;
            for (auto& node : scratch.gridNodes) node.generation = 0;
        }
        const uint32_t gen = scratch.generation;

        auto getIdx = [&](int rx, int rz, int sy) -> int {
            return sy * chunkGridSize + rz * diameter + rx;
        };

        scratch.curQueue.clear();
        scratch.nextQueue.clear();

        // Seed with the player's section
        const int startRX = job.renderDistance;
        const int startRZ = job.renderDistance;
        const int startSY = job.playerSectionY;
        const int startIdx = getIdx(startRX, startRZ, startSY);
        scratch.gridNodes[startIdx].generation = gen;
        scratch.gridNodes[startIdx].sourceDirections = 0x3F;

        scratch.curQueue.push_back({static_cast<int16_t>(startRX), static_cast<int16_t>(startRZ),
                                    static_cast<int8_t>(startSY), 0x3F});

        int visitedCount = 0;
        int occludedCount = 0;

        static constexpr int DIR_DX[]  = {0, 0, 0, 0, -1, 1};
        static constexpr int DIR_DZ[]  = {0, 0, -1, 1, 0, 0};
        static constexpr int DIR_DSY[] = {-1, 1, 0, 0, 0, 0};

        while (!scratch.curQueue.empty()) {
          for (const auto& entry : scratch.curQueue) {
            visitedCount++;

            const int idx = getIdx(entry.rx, entry.rz, entry.sy);
            GridNode& node = scratch.gridNodes[idx];
            const BfsJob::Cell& cell = job.cells[idx];

            const int worldCX = job.playerChunkX - job.renderDistance + entry.rx;
            const int worldCZ = job.playerChunkZ - job.renderDistance + entry.rz;
            const float sectionMinX = static_cast<float>(worldCX * 16);
            const float sectionMinY = static_cast<float>(entry.sy * 16 + Config::MinY);
            const float sectionMinZ = static_cast<float>(worldCZ * 16);

            // Every reachable section with geometry goes into the result —
            // the frustum is applied per frame by ChunkRenderer over this list.
            if (cell.gpuData) {
                // TRUE squared distance: orders both opaque front-to-back and
                // translucent back-to-front (no Y de-weighting — that's only
                // for load prioritization, see ClientChunkManager).
                const float dx = (sectionMinX + 8.0f) - cameraPos.x;
                const float dy = (sectionMinY + 8.0f) - cameraPos.y;
                const float dz = (sectionMinZ + 8.0f) - cameraPos.z;
                const float dist = dx * dx + dy * dy + dz * dz;

                SectionRenderData rd({worldCX, worldCZ}, entry.sy, cell.gpuData, dist);
                rd.layerMask = cell.layerMask;
                job.result.push_back(rd);
            }

            // VisibilitySet for traversal: the starting section is always
            // all-visible; everything else was baked into the snapshot
            // (meshed → real set, confirmed air → all, unmeshed solid → 0).
            VisibilitySet currentVis;
            const bool isStartingSection =
                (entry.rx == startRX && entry.rz == startRZ && entry.sy == startSY);
            if (isStartingSection) {
                currentVis.setAll(true);
            } else {
                currentVis.setRaw(cell.visBits);
            }

            // Distant LOS check flag — computed ONCE per current node (Minecraft-style)
            const bool distantFromCamera = smartCull && (
                std::abs(entry.rx - startRX) > 3 ||
                std::abs(entry.rz - startRZ) > 3 ||
                std::abs(entry.sy - startSY) > 3);

            for (int dir = 0; dir < 6; dir++) {
                const int nrx = entry.rx + DIR_DX[dir];
                const int nrz = entry.rz + DIR_DZ[dir];
                const int nsy = entry.sy + DIR_DSY[dir];

                if (nrx < 0 || nrx >= diameter || nrz < 0 || nrz >= diameter)
                    continue;
                if (nsy < 0 || nsy >= sectionsY)
                    continue;

                const int nIdx = getIdx(nrx, nrz, nsy);

                if (scratch.gridNodes[nIdx].generation == gen) {
                    scratch.gridNodes[nIdx].sourceDirections |= (1 << dir);
                    continue;
                }

                if (!blind && !job.chunkLoaded[nrz * diameter + nrx])
                    continue;

                // SMART CULL: check VisibilitySet
                if (smartCull) {
                    bool canReach = false;
                    const uint8_t srcDirs = node.sourceDirections;
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
                // section's corner toward the camera. If the ray passes through
                // any unvisited section, reject this neighbor.
                if (distantFromCamera) {
                    const float originX = sectionMinX;
                    const float originY = sectionMinY;
                    const float originZ = sectionMinZ;

                    const bool isXAxis = (dir == Direction::West || dir == Direction::East);
                    const bool isYAxis = (dir == Direction::Down || dir == Direction::Up);
                    const bool isZAxis = (dir == Direction::North || dir == Direction::South);
                    const bool mX = isXAxis ? (cameraPos.x > originX) : (cameraPos.x < originX);
                    const bool mY = isYAxis ? (cameraPos.y > originY) : (cameraPos.y < originY);
                    const bool mZ = isZAxis ? (cameraPos.z > originZ) : (cameraPos.z < originZ);

                    float ckX = originX + (mX ? 16.0f : 0.0f);
                    float ckY = originY + (mY ? 16.0f : 0.0f);
                    float ckZ = originZ + (mZ ? 16.0f : 0.0f);

                    const float rdx = cameraPos.x - ckX;
                    const float rdy = cameraPos.y - ckY;
                    const float rdz = cameraPos.z - ckZ;
                    const float rlen = std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz);

                    if (rlen > 0.001f) {
                        const float rscale = 28.0f / rlen;
                        const float sX = rdx * rscale, sY = rdy * rscale, sZ = rdz * rscale;
                        bool losVisible = true;

                        while ((ckX - cameraPos.x) * (ckX - cameraPos.x) +
                               (ckY - cameraPos.y) * (ckY - cameraPos.y) +
                               (ckZ - cameraPos.z) * (ckZ - cameraPos.z) > 3600.0f) {
                            ckX += sX; ckY += sY; ckZ += sZ;

                            if (ckY > static_cast<float>(Config::MaxY) ||
                                ckY < static_cast<float>(Config::MinY)) break;

                            const int rcx = static_cast<int>(std::floor(ckX / 16.0f));
                            int rsy = static_cast<int>(std::floor((ckY - Config::MinY) / 16.0f));
                            const int rcz = static_cast<int>(std::floor(ckZ / 16.0f));
                            rsy = std::clamp(rsy, 0, sectionsY - 1);
                            const int rrx = (rcx - job.playerChunkX) + job.renderDistance;
                            const int rrz = (rcz - job.playerChunkZ) + job.renderDistance;

                            if (rrx < 0 || rrx >= diameter || rrz < 0 || rrz >= diameter) {
                                losVisible = false;
                                break;
                            }
                            if (scratch.gridNodes[getIdx(rrx, rrz, rsy)].generation != gen) {
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

                scratch.gridNodes[nIdx].generation = gen;
                scratch.gridNodes[nIdx].sourceDirections = (1 << dir);

                scratch.nextQueue.push_back({static_cast<int16_t>(nrx), static_cast<int16_t>(nrz),
                                             static_cast<int8_t>(nsy),
                                             static_cast<uint8_t>(1 << dir)});
            }
          }
          scratch.curQueue.clear();
          std::swap(scratch.curQueue, scratch.nextQueue);
        }

        // Sort front-to-back off the main thread — the per-frame frustum
        // filter preserves this order (translucent iterates in reverse).
        if (job.result.size() > 1) {
            std::sort(job.result.begin(), job.result.end(),
                      [](const SectionRenderData& a, const SectionRenderData& b) {
                          return a.distanceToCamera < b.distanceToCamera;
                      });
        }

        job.visitedCount = visitedCount;
        job.occludedCount = occludedCount;
    }

} // namespace Render
