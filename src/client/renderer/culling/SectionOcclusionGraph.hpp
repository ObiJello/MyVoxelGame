// File: src/client/renderer/culling/SectionOcclusionGraph.hpp
// BFS-based occlusion culling that determines which sections are visible from
// the player's position by propagating through non-solid terrain faces.
// Mirrors Minecraft's SectionOcclusionGraph — sections behind solid terrain
// are never added to the render list, typically eliminating 60-70% of sections.
//
// ASYNC MODEL: the BFS itself is thread-agnostic and runs over a flat snapshot
// (BfsJob) built on the main thread by BuildInput(). ChunkRenderer either runs
// a job synchronously (cold start) or submits it to this class's dedicated
// worker thread and keeps rendering the previous result until the new one is
// collected — the main thread never pays the multi-millisecond BFS cost in
// steady operation. The worker touches ONLY the job's own snapshot data; it
// never dereferences GPUSectionData pointers or reads chunk-manager state
// (pointer safety across erases is handled by ChunkRenderer's erase token).
#pragma once

#include "VisibilitySet.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/core/Config.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace Render {

    struct SectionRenderData;
    struct GPUSectionData;

    class SectionOcclusionGraph {
    public:
        SectionOcclusionGraph() = default;
        ~SectionOcclusionGraph();

        // One visited-grid cell. Declared before BfsJob because the job now
        // carries the finished grid out as output.
        struct GridNodeOut {
            uint32_t generation = 0;
            uint8_t  sourceDirections = 0;
            // Whether this section has been pushed into the draw list for the
            // current generation. A section can be REACHED before it has been
            // meshed — it becomes a node with no geometry to emit — and must
            // still be emitted later when its mesh arrives. Without this flag
            // the partial update sees "already a node", propagates outward from
            // it, and never adds the section itself.
            uint8_t  emitted = 0;
        };

        // One BFS request: input snapshot + identity tokens + output.
        struct BfsJob {
            // --- Input (filled by BuildInput on the main thread) ---
            glm::vec3 cameraPos{0.0f};
            bool smartCull = true;
            int renderDistance = 0;
            int diameter = 0;   // 2*renderDistance + 1
            int playerChunkX = 0, playerChunkZ = 0, playerSectionY = 0;

            // Per-section snapshot cell. visBits encodes what the BFS needs to
            // traverse: a meshed section's VisibilitySet, all-visible for
            // confirmed-air sections, 0 (opaque) for unmeshed solid sections.
            // gpuData is carried into results for the render path only — the
            // worker NEVER dereferences it.
            struct Cell {
                // NO POINTER. The worker thread never needed to dereference
                // this — it only ever asked "is there geometry?" — and holding
                // a GPUSectionData* here was the last thing forcing the whole
                // tombstone/graveyard/build-counter apparatus to exist. The
                // draw path resolves live from (chunkPos, sectionY) instead.
                bool hasGeometry = false;
                // Chunk is loaded and the section is not all-air, i.e. it is a
                // thing that CAN be drawn once meshed. MC's inverse of
                // `loadedEmptySections` (SectionOcclusionGraph.runUpdates:253).
                bool renderable = false;
                uint64_t visBits = 0;
            };
            std::vector<Cell> cells;           // diameter^2 * SECTIONS_PER_CHUNK, [sy][rz][rx]
            std::vector<uint8_t> chunkLoaded;  // diameter^2, [rz][rx]

            // Identity / safety tokens (owned by ChunkRenderer)
            int keyCx = 0, keyCz = 0, keySy = 0;  // camera section this was built for
            uint64_t worldVersion = 0;             // staleness tracking
            uint64_t eraseToken = 0;               // pointer-safety: mismatched results are discarded

            // Whether the snapshot saw the camera's own chunk as LOADED.
            // When false, the BFS seed cannot spread (its chunk column is
            // absent from the snapshot) and an empty result says nothing
            // about the world — it must not replace a populated cache slot.
            // Happens when the player crosses into a chunk the server's
            // throttled stream hasn't delivered yet (the "sky flash" bug).
            bool centerLoaded = false;

            // --- Output (filled by the BFS) ---
            std::vector<SectionRenderData> result;  // reachable sections with geometry, sorted front-to-back
            int visitedCount = 0;
            int occludedCount = 0;

            // The visited grid the BFS ended with, kept as OUTPUT so a completed
            // job can seed the incremental graph (MC: scheduleFullUpdate hands
            // its finished GraphStorage to currentGraph). Sized
            // diameter^2 * SECTIONS_PER_CHUNK, same indexing as `cells`.
            std::vector<GridNodeOut> nodes;
            uint32_t generation = 0;   // value in nodes[] that means "visited"
        };

        // Fill a job's input snapshot from ClientChunkManager state.
        // MAIN THREAD ONLY (reads chunk map + section infos).
        void BuildInput(BfsJob& job, const glm::vec3& cameraPos,
                        bool smartCull, int renderDistance);

        // Run the BFS on the calling thread (cold-start fallback).
        void RunSync(BfsJob& job) { Run(job, m_syncScratch); }

        // --- Async API (single job in flight) ---
        bool Busy();                                  // pending, running, or uncollected result
        void SubmitAsync(std::unique_ptr<BfsJob> job);// requires !Busy()
        std::unique_ptr<BfsJob> TryCollect();         // non-blocking; null if nothing finished
        std::unique_ptr<BfsJob> AcquireJob();         // pooled allocation (reuses big buffers)
        void RecycleJob(std::unique_ptr<BfsJob> job); // return buffers to the pool

        // ── Incremental update (MC SectionOcclusionGraph.runPartialUpdate) ───
        //
        // MC runs a FULL graph rebuild asynchronously and only occasionally
        // (camera moved 8 blocks, or explicit invalidate), but propagates from
        // newly-compiled sections on the MAIN THREAD every single frame. A
        // section that finishes meshing becomes a BFS source next frame via
        // schedulePropagationFrom, so the reachable set grows immediately
        // instead of waiting for the next full rebuild.
        //
        // We previously had only the full rebuild — ~50-90 per second, each
        // with 1-2 frames of collect latency. Everything downstream that waits
        // on visibility inherited that as its ceiling.
        //
        // SAFETY: a partial update only ever ADDS sections, exactly as MC's
        // does (removal happens solely through a full rebuild). It therefore
        // cannot empty the draw list, which is what the "sky flash" failure
        // mode requires — see the centerLoaded/blind guards on the full path.

        // A section's mesh just became available — MC's
        // GraphEvents.sectionsToPropagateFrom. Cheap; safe to call per upload.
        void SchedulePropagationFrom(Game::Math::ChunkPos chunkPos, int sectionY);

        // Adopt a completed full-rebuild job's grid as the live graph.
        // Call right after taking ownership of the job's result.
        void AdoptGraph(const BfsJob& job);

        // Drop the graph (camera section changed, pointers erased, …). The next
        // full rebuild re-establishes it; partial updates no-op until then.
        void InvalidateGraph();

        // MC runPartialUpdate. Propagates from every queued section into the
        // live graph and APPENDS newly-reachable sections to `outSections`.
        // Returns true if anything was added. MAIN THREAD ONLY — it reads
        // ClientChunkManager state and GPUSectionData pointers directly.
        //
        // Does nothing unless the graph is valid AND anchored to this exact
        // camera section / render distance / erase token, so a stale graph can
        // never contribute sections for the wrong viewpoint.
        bool RunPartialUpdate(const glm::vec3& cameraPos,
                              int cameraChunkX, int cameraChunkZ, int cameraSectionY,
                              int renderDistance, uint64_t eraseToken,
                              std::vector<SectionRenderData>& outSections);

        bool HasGraphFor(int cameraChunkX, int cameraChunkZ, int cameraSectionY,
                         int renderDistance, uint64_t eraseToken) const;

    private:
        // Per-run scratch (visited grid + BFS queues). One instance for the
        // sync path and one owned by the worker so a cold-start sync run can
        // never race an in-flight async run.
        struct QueueEntry {
            int16_t rx, rz;
            int8_t sy;
            uint8_t sourceDirections;
        };
        struct Scratch {
            std::vector<QueueEntry> curQueue;
            std::vector<QueueEntry> nextQueue;
        };

        // The live graph — MC's GraphState.currentGraph. Holds the visited grid
        // a full rebuild ended with, so partial updates can extend it.
        struct LiveGraph {
            bool valid = false;
            int cx = 0, cz = 0, sy = 0;      // anchor camera section
            int renderDistance = 0, diameter = 0;
            bool smartCull = true;
            bool blind = false;
            uint64_t eraseToken = 0;
            glm::vec3 cameraPos{0.0f};
            std::vector<GridNodeOut> nodes;  // parallel to a job's `nodes`
            uint32_t generation = 0;
        };
        LiveGraph m_graph;

        // MC's GraphEvents.sectionsToPropagateFrom. Main-thread only: mesh
        // uploads and PrepareVisibleSections both run there.
        struct PendingSource { Game::Math::ChunkPos chunkPos; int sectionY; };
        std::vector<PendingSource> m_propagateFrom;
        Scratch m_partialScratch;

        // The BFS itself — pure function of (job input, scratch).
        static void Run(BfsJob& job, Scratch& scratch);

        void EnsureWorkerStarted();
        void WorkerLoop();

        Scratch m_syncScratch;
        Scratch m_workerScratch;

        std::thread m_worker;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::unique_ptr<BfsJob> m_pending;    // submitted, worker not started on it yet
        std::unique_ptr<BfsJob> m_completed;  // finished, awaiting TryCollect
        std::unique_ptr<BfsJob> m_pool;       // recycled job (buffer reuse)
        bool m_running = false;               // worker currently executing a job
        bool m_shutdown = false;

    };

} // namespace Render
