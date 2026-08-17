// File: src/client/renderer/culling/SectionOcclusionGraph.cpp
#include "SectionOcclusionGraph.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/SectionMesh.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "common/core/ThreadPriority.hpp"
#include "common/core/Config.hpp"
#include <algorithm>
#include <cmath>
#include "common/world/math/ChunkViewDistance.hpp"

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

                // MC isInViewDistance (buffer 1) — what the client RENDERS, as
                // opposed to what the server SENDS (buffer 2). The outermost
                // ring of loaded chunks is a halo: it is traversed by the BFS
                // below, but never drawn.
                //
                // This is not cosmetic. A section is only admitted for its first
                // compile once all 8 neighbouring columns exist
                // (ClientChunkManager::HasAllNeighborChunks, MC
                // hasAllNeighbors). Emitting the outer ring as renderable would
                // queue sections whose neighbours are never sent, so they could
                // never satisfy the gate and would stay permanently unmeshed —
                // measured at 76 empty chunks ringing the horizon at view
                // distance 16. Rendering the buffer-1 set makes the invariant
                // hold by construction.
                const bool inViewDistance = Game::Math::IsWithinChunkViewDistance(
                    job.playerChunkX, job.playerChunkZ, renderDistance, cx, cz,
                    /*includeNeighbors=*/false);

                for (int sy = 0; sy < sectionsY; ++sy) {
                    BfsJob::Cell& cell = job.cells[static_cast<size_t>(sy) * chunkGridSize + ci];
                    const auto& si = chunk->sectionInfos[sy];
                    // MC's emptySections test: an all-air section is traversable
                    // but is never a draw candidate; everything else is.
                    cell.renderable = inViewDistance && !si.isAllAir;
                    GPUSectionData* gpu = si.gpuData.load(std::memory_order_acquire);
                    if (gpu && gpu->HasGeometry()) {
                        // The pointer is dereferenced HERE, on the main thread,
                        // and only its visibility bits are carried forward. The
                        // worker never sees a pointer.
                        cell.hasGeometry = true;
                        cell.visBits = gpu->visibilitySet.raw();
                    } else if (si.isAllAir) {
                        cell.visBits = VisibilitySet::kAllVisibleBits;
                    }
                    // else: non-air but unmeshed — visBits 0 blocks BFS, exactly
                    // like MC's CompiledSectionMesh.UNCOMPILED, whose
                    // facesCanSeeEachother() returns false.
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
            m_pending = std::move(job);
        }
        m_cv.notify_one();
    }

    std::unique_ptr<SectionOcclusionGraph::BfsJob> SectionOcclusionGraph::TryCollect() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::move(m_completed);
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
        // The frame renders the previous result while this runs, but a late BFS
        // means rendering a stale visibility set — short deadline, not throughput.
        Core::SetCurrentThreadPriority(Core::ThreadPriorityClass::Elevated);
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

    // ========================================================================
    // INCREMENTAL GRAPH (MC SectionOcclusionGraph partial update)
    // ========================================================================

    void SectionOcclusionGraph::SchedulePropagationFrom(Game::Math::ChunkPos chunkPos, int sectionY) {
        // MC's schedulePropagationFrom. Queued unconditionally and filtered at
        // drain time — the graph may be mid-rebuild right now, and a source
        // that lands outside the eventual graph is simply skipped there.
        if (sectionY < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) return;
        // Cheap consecutive-duplicate filter. A section being remeshed several
        // times in a row is the common repeat, and now that sources survive an
        // anchor mismatch the queue can sit unread for a while. Full dedup would
        // cost more than the duplicate does — draining a source that is already
        // a node just re-propagates from it, which is idempotent.
        if (!m_propagateFrom.empty()) {
            const PendingSource& last = m_propagateFrom.back();
            if (last.sectionY == sectionY && last.chunkPos == chunkPos) return;
        }
        m_propagateFrom.push_back({chunkPos, sectionY});
    }

    void SectionOcclusionGraph::InvalidateGraph() {
        m_graph.valid = false;
        m_propagateFrom.clear();
    }

    void SectionOcclusionGraph::AdoptGraph(const BfsJob& job) {
        // Mirrors MC's currentGraph.set(newState) at the end of a full update.
        //
        // A BLIND run never becomes the live graph. Blind means the snapshot
        // did not contain the camera's own chunk, so the BFS degraded to
        // "traverse everything, skip occlusion" — its visited grid does not
        // describe reachability and extending it would propagate that fiction
        // frame after frame. Waiting for a real rebuild is correct here; the
        // caller keeps rendering the previous slot meanwhile.
        //
        // (job.result is NOT consulted: the caller swaps it into its slot
        // before calling, so it is always empty by now.)
        if (job.nodes.empty() || !job.centerLoaded) {
            m_graph.valid = false;
            return;
        }
        m_graph.valid          = true;
        m_graph.cx             = job.keyCx;
        m_graph.cz             = job.keyCz;
        m_graph.sy             = job.keySy;
        m_graph.renderDistance = job.renderDistance;
        m_graph.diameter       = job.diameter;
        m_graph.smartCull      = job.smartCull;
        m_graph.blind          = !job.centerLoaded;
        m_graph.eraseToken     = job.eraseToken;
        m_graph.cameraPos      = job.cameraPos;
        m_graph.nodes          = job.nodes;
        m_graph.generation     = job.generation;

        // Queued sources are KEPT, not dropped. MC does the same and is
        // explicit about it — schedulePropagationFrom pushes into
        // nextGraphEvents (the in-flight rebuild's event queue) as well as the
        // current graph's, so whatever compiled during a rebuild is already
        // waiting when the new graph goes live.
        //
        // Dropping them here was a real bug: full rebuilds land 37-76 times a
        // second while streaming, so most sections that finished meshing had
        // their propagation source discarded before any frame drained it, and
        // the incremental path only ever fired on ~42% of frames. Sources that
        // do not correspond to a node in the NEW graph are filtered harmlessly
        // in RunPartialUpdate.
    }

    bool SectionOcclusionGraph::HasGraphFor(int cameraChunkX, int cameraChunkZ, int cameraSectionY,
                                            int renderDistance, uint64_t eraseToken) const {
        return m_graph.valid &&
               m_graph.cx == cameraChunkX && m_graph.cz == cameraChunkZ &&
               m_graph.sy == cameraSectionY &&
               m_graph.renderDistance == renderDistance &&
               m_graph.eraseToken == eraseToken;
    }

    bool SectionOcclusionGraph::RunPartialUpdate(const glm::vec3& cameraPos,
                                                 int cameraChunkX, int cameraChunkZ, int cameraSectionY,
                                                 int renderDistance, uint64_t eraseToken,
                                                 std::vector<SectionRenderData>& outSections) {
        PROFILE_ZONE_N("OcclusionPartial");

        // Did the incremental path actually get to run this frame? Every
        // regression in this file so far has been the answer being "no" far
        // more often than assumed, so it is measured rather than reasoned about.
        if (m_propagateFrom.empty()) {
            PROFILE_PLOT("Occlusion/PartialRan", static_cast<int64_t>(0));
            return false;
        }
        if (!HasGraphFor(cameraChunkX, cameraChunkZ, cameraSectionY, renderDistance, eraseToken) ||
            !Client::g_clientChunkManager) {
            // Graph is for another viewpoint (or missing). KEEP the sources.
            //
            // MC keeps them too, and is explicit about it —
            // schedulePropagationFrom:109-120 pushes into BOTH the current
            // graph's event queue and the in-flight rebuild's, so nothing
            // compiled during a rebuild is lost.
            //
            // Clearing here was a real defect: our anchor is the EXACT camera
            // section INCLUDING sy, so simply walking across any 16-block
            // boundary landed in this branch and threw away every pending
            // source. Sources are stored in world coordinates and re-derived
            // against whatever anchor is live when they are finally drained
            // (see the rx/rz conversion below), so holding them costs nothing
            // and they remain correct for the next graph.
            //
            // The cap is the only backstop: if the graph stays mismatched long
            // enough to accumulate this many, a full rebuild is cheaper than
            // the backlog, so ask for one and start clean.
            static constexpr size_t kMaxPendingSources = 4096;
            if (m_propagateFrom.size() > kMaxPendingSources) {
                PROFILE_PLOT("Occlusion/SourcesDropped",
                             static_cast<int64_t>(m_propagateFrom.size()));
                m_propagateFrom.clear();
            } else {
                PROFILE_PLOT("Occlusion/SourcesDropped", static_cast<int64_t>(0));
            }
            PROFILE_PLOT("Occlusion/PartialRan", static_cast<int64_t>(0));
            return false;
        }
        PROFILE_PLOT("Occlusion/PartialRan", static_cast<int64_t>(1));
        PROFILE_PLOT("Occlusion/SourcesDropped", static_cast<int64_t>(0));

        const int diameter  = m_graph.diameter;
        const int sectionsY = Game::Math::SECTIONS_PER_CHUNK;
        const int chunkGrid = diameter * diameter;
        const uint32_t gen  = m_graph.generation;

        auto getIdx = [&](int rx, int rz, int sy) { return sy * chunkGrid + rz * diameter + rx; };

        // Live per-cell read — the snapshot the full rebuild used is long gone.
        // Safe here and ONLY here because this runs on the main thread, which
        // is also where meshes are uploaded and GPUSectionData is erased.
        struct LiveCell { uint64_t visBits; bool loaded; bool renderable; };
        auto fetchCell = [&](int rx, int rz, int sy) -> LiveCell {
            LiveCell c{0, false, false};
            const int cx = m_graph.cx - m_graph.renderDistance + rx;
            const int cz = m_graph.cz - m_graph.renderDistance + rz;
            const Client::ClientChunk* chunk = Client::g_clientChunkManager->GetChunk({cx, cz});
            if (!chunk || chunk->state != Client::ChunkState::LOADED) return c;
            c.loaded = true;
            const auto& si = chunk->sectionInfos[sy];
            c.renderable = !si.isAllAir &&
                Game::Math::IsWithinChunkViewDistance(m_graph.cx, m_graph.cz,
                                                      m_graph.renderDistance, cx, cz,
                                                      /*includeNeighbors=*/false);
            GPUSectionData* gpu = si.gpuData.load(std::memory_order_acquire);
            if (gpu && gpu->HasGeometry()) {
                c.visBits = gpu->visibilitySet.raw();
            } else if (si.isAllAir) {
                c.visBits = VisibilitySet::kAllVisibleBits;
            }
            return c;
        };

        // Seed from the queued sections, MC runPartialUpdate:154-160: a source
        // only counts if it is ALREADY a node in this graph. A section the BFS
        // never reached cannot start a propagation — it would invent
        // reachability that the occlusion test never granted.
        Scratch& scratch = m_partialScratch;
        scratch.curQueue.clear();
        scratch.nextQueue.clear();

        static constexpr int SEED_DX[]  = {0, 0, 0, 0, -1, 1};
        static constexpr int SEED_DZ[]  = {0, 0, -1, 1, 0, 0};
        static constexpr int SEED_DSY[] = {-1, 1, 0, 0, 0, 0};

        for (const PendingSource& src : m_propagateFrom) {
            const int rx = (src.chunkPos.x - m_graph.cx) + m_graph.renderDistance;
            const int rz = (src.chunkPos.z - m_graph.cz) + m_graph.renderDistance;
            if (rx < 0 || rx >= diameter || rz < 0 || rz >= diameter) continue;
            if (src.sectionY < 0 || src.sectionY >= sectionsY) continue;

            const int idx = getIdx(rx, rz, src.sectionY);
            if (m_graph.nodes[idx].generation == gen) {
                // Already a node — propagate outward from it, as MC does with
                // sectionToNodeMap.get(renderSection).
                scratch.curQueue.push_back({static_cast<int16_t>(rx), static_cast<int16_t>(rz),
                                            static_cast<int8_t>(src.sectionY),
                                            m_graph.nodes[idx].sourceDirections});

                // ...but a node is not necessarily IN the draw list. A node is
                // created for every traversed cell, while only `renderable`
                // ones are emitted — so a section whose chunk had not streamed
                // in yet, or that was all-air at the time, is a node with no
                // list entry. If it has since become renderable, emit it now;
                // otherwise it stays reachable-but-invisible until the next full
                // rebuild, which is the "chunks appear late" symptom.
                if (!m_graph.nodes[idx].emitted) {
                    const LiveCell self = fetchCell(rx, rz, src.sectionY);
                    if (self.renderable) {
                        const float minX = static_cast<float>((m_graph.cx - m_graph.renderDistance + rx) * 16);
                        const float minY = static_cast<float>(src.sectionY * 16 + Config::MinY);
                        const float minZ = static_cast<float>((m_graph.cz - m_graph.renderDistance + rz) * 16);
                        const float dx = (minX + 8.0f) - cameraPos.x;
                        const float dy = (minY + 8.0f) - cameraPos.y;
                        const float dz = (minZ + 8.0f) - cameraPos.z;
                        outSections.emplace_back(
                            Game::Math::ChunkPos{m_graph.cx - m_graph.renderDistance + rx,
                                                 m_graph.cz - m_graph.renderDistance + rz},
                            src.sectionY, dx * dx + dy * dy + dz * dz);
                        m_graph.nodes[idx].emitted = 1;
                    }
                }
                continue;
            }

            // Not in the graph. This is the streaming case: the section's chunk
            // was not loaded when the last full rebuild ran, so the BFS never
            // reached it. MC handles this with chunksWaitingForNeighbors +
            // onChunkReadyToRender, which re-queues such sections once their
            // neighbours exist. We get the same effect by seeding from any
            // NEIGHBOUR that is already a node — propagation then flows from
            // the established graph into the new section on this very pass.
            //
            // Without this, a streamed-in section could never enter the graph
            // except via a full rebuild, which is exactly the coupling we are
            // trying to remove.
            for (int d = 0; d < 6; ++d) {
                const int nrx = rx + SEED_DX[d];
                const int nrz = rz + SEED_DZ[d];
                const int nsy = src.sectionY + SEED_DSY[d];
                if (nrx < 0 || nrx >= diameter || nrz < 0 || nrz >= diameter) continue;
                if (nsy < 0 || nsy >= sectionsY) continue;
                const int nIdx = getIdx(nrx, nrz, nsy);
                if (m_graph.nodes[nIdx].generation != gen) continue;
                scratch.curQueue.push_back({static_cast<int16_t>(nrx), static_cast<int16_t>(nrz),
                                            static_cast<int8_t>(nsy),
                                            m_graph.nodes[nIdx].sourceDirections});
            }
        }
        m_propagateFrom.clear();
        if (scratch.curQueue.empty()) return false;

        static constexpr int DIR_DX[]  = {0, 0, 0, 0, -1, 1};
        static constexpr int DIR_DZ[]  = {0, 0, -1, 1, 0, 0};
        static constexpr int DIR_DSY[] = {-1, 1, 0, 0, 0, 0};

        const size_t addedBefore = outSections.size();

        while (!scratch.curQueue.empty()) {
            for (const auto& entry : scratch.curQueue) {
                const LiveCell cell = fetchCell(entry.rx, entry.rz, entry.sy);

                VisibilitySet vis;
                vis.setRaw(cell.visBits);

                for (int dir = 0; dir < 6; dir++) {
                    const int nrx = entry.rx + DIR_DX[dir];
                    const int nrz = entry.rz + DIR_DZ[dir];
                    const int nsy = entry.sy + DIR_DSY[dir];
                    if (nrx < 0 || nrx >= diameter || nrz < 0 || nrz >= diameter) continue;
                    if (nsy < 0 || nsy >= sectionsY) continue;

                    const int nIdx = getIdx(nrx, nrz, nsy);
                    if (m_graph.nodes[nIdx].generation == gen) {
                        // Already reachable — just record the extra approach
                        // direction, MC's existingNode.addSourceDirection.
                        m_graph.nodes[nIdx].sourceDirections |= (1 << dir);
                        continue;
                    }

                    if (m_graph.smartCull) {
                        bool canReach = false;
                        for (int srcBit = 0; srcBit < 6; srcBit++) {
                            if (!(entry.sourceDirections & (1 << srcBit))) continue;
                            if (vis.canSeeThrough(Direction::opposite(srcBit), dir)) { canReach = true; break; }
                        }
                        if (!canReach) continue;
                    }

                    const LiveCell ncell = fetchCell(nrx, nrz, nsy);
                    if (!m_graph.blind && !ncell.loaded) continue;

                    m_graph.nodes[nIdx].generation = gen;
                    m_graph.nodes[nIdx].sourceDirections = static_cast<uint8_t>(1 << dir);
                    m_graph.nodes[nIdx].emitted = 0;

                    if (ncell.renderable) {
                        const float minX = static_cast<float>((m_graph.cx - m_graph.renderDistance + nrx) * 16);
                        const float minY = static_cast<float>(nsy * 16 + Config::MinY);
                        const float minZ = static_cast<float>((m_graph.cz - m_graph.renderDistance + nrz) * 16);
                        const float dx = (minX + 8.0f) - cameraPos.x;
                        const float dy = (minY + 8.0f) - cameraPos.y;
                        const float dz = (minZ + 8.0f) - cameraPos.z;
                        outSections.emplace_back(
                            Game::Math::ChunkPos{m_graph.cx - m_graph.renderDistance + nrx,
                                                 m_graph.cz - m_graph.renderDistance + nrz},
                            nsy, dx * dx + dy * dy + dz * dz);
                        m_graph.nodes[nIdx].emitted = 1;
                    }

                    scratch.nextQueue.push_back({static_cast<int16_t>(nrx), static_cast<int16_t>(nrz),
                                                 static_cast<int8_t>(nsy),
                                                 static_cast<uint8_t>(1 << dir)});
                }
            }
            scratch.curQueue.clear();
            std::swap(scratch.curQueue, scratch.nextQueue);
        }

        // Deliberately NOT re-running the distant line-of-sight raycast the full
        // rebuild does. That test only ever REMOVES sections, so skipping it
        // makes partial updates permissive: they may admit a few sections the
        // full pass would have culled, never hide one it would have kept. The
        // next full rebuild is authoritative and takes them back out. Being
        // permissive is the safe direction — the failure mode we care about is
        // an empty draw list, and a partial update can only grow it.
        // The gate number for visible-only meshing: how many sections per second
        // the incremental path can admit on its own. It must out-supply the
        // dirty-set scheduler BEFORE it is allowed to replace it.
        PROFILE_PLOT("Occlusion/PartialAdded",
                     static_cast<int64_t>(outSections.size() - addedBefore));

        return outSections.size() != addedBefore;
    }

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
        job.nodes.resize(gridSize);
        job.generation++;
        if (job.generation == 0) {
            job.generation = 1;
            for (auto& node : job.nodes) node.generation = 0;
        }
        const uint32_t gen = job.generation;

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
        job.nodes[startIdx].generation = gen;
        job.nodes[startIdx].sourceDirections = 0x3F;
        job.nodes[startIdx].emitted = 0;

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
            GridNodeOut& node = job.nodes[idx];
            const BfsJob::Cell& cell = job.cells[idx];

            const int worldCX = job.playerChunkX - job.renderDistance + entry.rx;
            const int worldCZ = job.playerChunkZ - job.renderDistance + entry.rz;
            const float sectionMinX = static_cast<float>(worldCX * 16);
            const float sectionMinY = static_cast<float>(entry.sy * 16 + Config::MinY);
            const float sectionMinZ = static_cast<float>(worldCZ * 16);

            // Every reachable NON-EMPTY section is emitted — meshed or not.
            // MC does exactly this: runUpdates:253-257 adds every polled node to
            // the octree unless it is in loadedEmptySections, and
            // addSectionsInFrustum:79-90 copies all of them into visibleSections.
            // Compile state is not a membership test there and is not one here.
            //
            // Emitting the unmeshed ones is what lets this list drive mesh
            // scheduling. An earlier attempt gated membership on having geometry
            // and then fed the list to the scheduler, which meant only re-meshes
            // were ever scheduled and the frontier never advanced.
            //
            // Carrying no pointer is what makes this safe: an entry for a
            // section that is not (or no longer) meshed simply resolves to null
            // at draw time and contributes nothing, exactly like MC's
            // getBuffers(layer) returning null.
            if (cell.renderable) {
                // TRUE squared distance: orders both opaque front-to-back and
                // translucent back-to-front (no Y de-weighting — that's only
                // for load prioritization, see ClientChunkManager).
                const float dx = (sectionMinX + 8.0f) - cameraPos.x;
                const float dy = (sectionMinY + 8.0f) - cameraPos.y;
                const float dz = (sectionMinZ + 8.0f) - cameraPos.z;
                const float dist = dx * dx + dy * dy + dz * dz;

                job.result.emplace_back(Game::Math::ChunkPos{worldCX, worldCZ}, entry.sy, dist);
                job.nodes[idx].emitted = 1;
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

                if (job.nodes[nIdx].generation == gen) {
                    job.nodes[nIdx].sourceDirections |= (1 << dir);
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
                            if (job.nodes[getIdx(rrx, rrz, rsy)].generation != gen) {
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

                job.nodes[nIdx].generation = gen;
                job.nodes[nIdx].sourceDirections = (1 << dir);
                job.nodes[nIdx].emitted = 0;

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
