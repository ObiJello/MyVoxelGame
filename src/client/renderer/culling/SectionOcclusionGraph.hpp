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
                GPUSectionData* gpuData = nullptr;
                uint64_t visBits = 0;
                uint8_t layerMask = 0;
            };
            std::vector<Cell> cells;           // diameter^2 * SECTIONS_PER_CHUNK, [sy][rz][rx]
            std::vector<uint8_t> chunkLoaded;  // diameter^2, [rz][rx]

            // Identity / safety tokens (owned by ChunkRenderer)
            int keyCx = 0, keyCz = 0, keySy = 0;  // camera section this was built for
            uint64_t worldVersion = 0;             // staleness tracking
            uint64_t eraseToken = 0;               // pointer-safety: mismatched results are discarded
            uint32_t buildCounter = 0;             // prepare-counter at snapshot time (tombstone reclamation)

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

        // buildCounter of the job currently in the pipeline (pending, running
        // or awaiting collection), if any. Its snapshot holds GPUSectionData
        // pointers, so tombstone reclamation must treat it as a live reader.
        bool InFlightBuildCounter(uint32_t& out);

    private:
        // Per-run scratch (visited grid + BFS queues). One instance for the
        // sync path and one owned by the worker so a cold-start sync run can
        // never race an in-flight async run.
        struct GridNode {
            uint32_t generation;
            uint8_t sourceDirections;
        };
        struct QueueEntry {
            int16_t rx, rz;
            int8_t sy;
            uint8_t sourceDirections;
        };
        struct Scratch {
            std::vector<GridNode> gridNodes;
            uint32_t generation = 0;
            std::vector<QueueEntry> curQueue;
            std::vector<QueueEntry> nextQueue;
        };

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

        // Tracks the pipeline job's buildCounter from SubmitAsync until
        // TryCollect hands it back (covers the running phase, when the job
        // pointer itself is owned by the worker). At most one job is in the
        // pipeline at a time (SubmitAsync requires !Busy()).
        bool m_inFlightActive = false;
        uint32_t m_inFlightBuildCounter = 0;
    };

} // namespace Render
