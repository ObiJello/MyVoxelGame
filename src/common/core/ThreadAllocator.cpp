// File: src/common/core/ThreadAllocator.cpp
#include "ThreadAllocator.hpp"
#include "HardwareProfile.hpp"
#include "Log.hpp"
#include <sstream>
#include <cstdint>
#include <algorithm>
#include <thread>

namespace Core {

    std::string ThreadAllocation::ToString() const {
        std::stringstream ss;
        ss << "Thread Allocation: "
           << "Total Cores=" << totalCores
           << " (Performance=" << performanceCores << ")"
           << ", Reserved=" << reservedThreads
           << ", Available Workers=" << availableWorkers
           << ", Client Mesh Workers=" << clientMeshWorkers
           << ", Server World Workers=" << serverWorldWorkers;
        return ss.str();
    }

    ThreadAllocation ThreadAllocator::GetOptimalAllocation() {
        ThreadAllocation allocation{};
        
        // Get total CPU cores
        allocation.totalCores = GetPhysicalCoreCount();
        allocation.performanceCores = GetPerformanceCoreCount();
        allocation.reservedThreads = TOTAL_RESERVED;
        
        // Calculate available worker threads
        if (allocation.totalCores > allocation.reservedThreads) {
            allocation.availableWorkers = allocation.totalCores - allocation.reservedThreads;
        } else {
            // Not enough cores, use minimum allocation
            allocation.availableWorkers = 2; // Minimum 1 per pool
        }
        
        // Distribute workers between client and server
        DistributeWorkers(allocation);
        
        // Validate and adjust if needed
        ValidateAllocation(allocation);
        
        Log::Info("Thread Allocation: %zu cores detected (%zu performance), %zu reserved, %zu available for workers",
                  allocation.totalCores, allocation.performanceCores,
                  allocation.reservedThreads, allocation.availableWorkers);
        Log::Info("  Client Mesh Workers: %zu", allocation.clientMeshWorkers);
        Log::Info("  Server World Workers: %zu", allocation.serverWorldWorkers);
        
        return allocation;
    }

    ThreadAllocation ThreadAllocator::GetOptimalAllocation(size_t manualClientWorkers, 
                                                           size_t manualServerWorkers) {
        ThreadAllocation allocation{};
        
        allocation.totalCores = GetPhysicalCoreCount();
        allocation.performanceCores = GetPerformanceCoreCount();
        allocation.reservedThreads = TOTAL_RESERVED;
        allocation.clientMeshWorkers = manualClientWorkers;
        allocation.serverWorldWorkers = manualServerWorkers;
        allocation.availableWorkers = manualClientWorkers + manualServerWorkers;
        
        // Validate manual allocation
        ValidateAllocation(allocation);
        
        Log::Info("Thread Allocation (Manual Override): %zu cores detected", allocation.totalCores);
        Log::Info("  Client Mesh Workers: %zu (manual)", allocation.clientMeshWorkers);
        Log::Info("  Server World Workers: %zu (manual)", allocation.serverWorldWorkers);
        
        return allocation;
    }

    size_t ThreadAllocator::GetPhysicalCoreCount() {
        // HardwareProfile already substitutes DEFAULT_FALLBACK_CORES when
        // hardware_concurrency() reports 0.
        return HardwareProfile::Get().logicalCores;
    }

    size_t ThreadAllocator::GetPerformanceCoreCount() {
        // Apple Silicon is heterogeneous and hardware_concurrency() counts every
        // core equally — an M4 reports 10 when only 4 of them are performance
        // cores. Sizing latency-sensitive pools off that number is how you end
        // up with more frame-critical workers than fast cores to run them on.
        // HardwareProfile reads hw.perflevel0 for the real number and falls
        // back to the homogeneous answer everywhere else.
        return HardwareProfile::Get().performanceCores;
    }

    bool ThreadAllocator::HasSufficientCores() {
        size_t cores = GetPhysicalCoreCount();
        // We need at least reserved threads + 2 workers (1 per pool)
        return cores >= (TOTAL_RESERVED + 2);
    }

    void ThreadAllocator::DistributeWorkers(ThreadAllocation& allocation) {
        // ── Small machines first ────────────────────────────────────────────
        //
        // Everything below this block was tuned on a 10-core M4 and, before
        // this existed, applied its floors to every machine: a 2-core/4-thread
        // Intel MacBook got ONE mesh worker (correct) and then FOUR server
        // workers from the clamp at the bottom — more worker threads than it
        // has hardware threads, before the render thread, the server tick,
        // the terrain library's own pool, the occlusion thread and the I/O
        // threads are counted. Fourteen threads on two cores, and the frame
        // lost every scheduling contest to work that could have waited.
        //
        // MC's rule is a single background pool of clamp(cores - 1, 1, ...)
        // (Util.maxAllowedExecutorThreads) with no floors anywhere, and it
        // deliberately skips its thread-priority boosts on <= 4 cores so the
        // pool cannot starve the game thread (Minecraft.run:849). The
        // budget here is the same total — cores - 1 — split between the two
        // pools, with the server pool (disk loads, conversions, saves: work
        // that can wait a frame) taking the smaller share.
        //
        //   logical cores   mesh   server   (+3 reserved: render, tick, I/O)
        //   2               1      1
        //   3-4             1      2
        //   5-6             2      2
        //   7+              unchanged from the tuned split below
        //
        // Machines with 7+ threads keep the exact numbers they had, so a
        // fast machine's allocation is untouched by this branch.
        if (allocation.totalCores <= 6) {
            allocation.clientMeshWorkers  = allocation.totalCores >= 5 ? 2 : 1;
            allocation.serverWorldWorkers = allocation.totalCores >= 3 ? 2 : 1;
            return;
        }

        // Strategy: 50/50 split between client and server for balanced performance
        if (allocation.availableWorkers <= 2) {
            // Minimum allocation
            allocation.clientMeshWorkers = 1;
            allocation.serverWorldWorkers = std::max(size_t(1), allocation.availableWorkers - 1);
        } else {
            // Equal split for all systems (no cap on client workers)
            // Client mesh building is critical for smooth gameplay
            // Mesh workers get the larger share: chunk arrival is ~1,300/s on a
            // saved area now and meshing was the next limiter (2026-08-30);
            // the server pool is clamped to 4..6 below regardless, and both
            // pools idle when their queue is empty.
            allocation.clientMeshWorkers = std::max(size_t(3), allocation.availableWorkers - 2);
            allocation.serverWorldWorkers = std::max(size_t(4), allocation.availableWorkers - allocation.clientMeshWorkers);
        }

        // Heterogeneous-CPU correction. The split above counts efficiency cores
        // as if they were performance cores, which is fine for the SERVER pool
        // — terrain generation is throughput work and runs perfectly well on an
        // E-core (and is pinned to Throughput QoS to encourage exactly that).
        //
        // It is NOT fine for the mesh pool. Mesh builds sit on the critical path
        // to a drawn frame, so they are Elevated QoS and want real performance
        // cores. Letting that pool grow past the performance-core count just
        // creates threads that contend with the frame for the same few cores.
        // One core is left for the main thread, which outranks both.
        if (allocation.performanceCores > 0 && allocation.performanceCores < allocation.totalCores) {
            // +1 over the performance cores (was -1): a saved area arrives at
            // ~1,300 chunks/s now and meshing was the next limiter (2026-08-30).
            const size_t meshCap = std::max(size_t(1), allocation.performanceCores + 1);
            if (allocation.clientMeshWorkers > meshCap) {
                // Reassign rather than discard: the surplus is still useful as
                // throughput work on the efficiency cores.
                allocation.serverWorldWorkers += allocation.clientMeshWorkers - meshCap;
                allocation.clientMeshWorkers = meshCap;
            }
        }

        // ── Server pool is sized for CONCURRENCY, not cores ────────────────
        //
        // The comment above about terrain generation being throughput work that
        // suits an E-core describes a workload these threads do not run. A
        // server worker handling a chunk spends ~98% of its time ASLEEP inside
        // ServerChunkCache::getChunk (measured: 47.20 ms of a 47.84 ms load),
        // polling a future while the terrain library's OWN pool — the
        // BackgroundExecutor in MyTerrainGenerator, hardware_concurrency()-1
        // threads that ThreadAllocator never sees — does the actual work.
        //
        // So this pool is not a CPU budget, it is an in-flight-chunk limit.
        // Sizing it by cores capped concurrent chunk loads at 4: measured, 1802
        // of 1819 loads ran at exactly 4-way concurrency (pinned) while the 9
        // terrain threads sat ~90% idle. That is the real ceiling on how fast
        // chunks fill in, and it is why halving per-chunk latency was not
        // noticeable — the burst rate stayed pinned at workers / latency.
        //
        // Sized to the burst rate the pipeline can actually absorb, NOT to the
        // terrain pool. Measured: raising this to 11 lifted concurrency from 4 to
        // 11 but bought only +18% capacity (83.6 -> 98.9 chunks/s) while tripling
        // per-chunk latency (47.8 -> 111.2 ms), because the extra chunks just
        // queue. The terrain pool stays ~29% busy even during bursts and would
        // not saturate until ~230 chunks/s, so chunk delivery is not
        // thread-starved at all — it is bounded by the terrain library's chunk
        // DEPENDENCY GRAPH (a chunk waits on its neighbours at lower statuses).
        //
        // Observed burst peak is ~116 chunks/s, which at the 4-worker latency of
        // 47.8 ms needs ~5.5 slots in flight. Six covers the peak at close to
        // minimum latency; more only adds queueing delay. Do not raise this to
        // chase throughput — measure the dependency graph instead.
        //
        // UPDATE 2026-08-29: the "dependency graph" ceiling above was really
        // the terrain library's single-lane worldgen executor, where surface,
        // carvers, features and full ran serialised (~9.6 ms per chunk = ~100
        // chunks/s no matter what). Surface and carvers now run on the pool
        // (ChunkStatusTasks.h), which left the lane ~50% busy and the pool
        // ~50% busy with 6 slots — i.e. the in-flight count became the
        // limiter again. Twelve slots; re-measure before raising further.
        //
        // UPDATE 2026-08-30: workers no longer block inside the library at all
        // — generation is ticket-driven (ServerWorkerPool::ProcessChunkLoading
        // hands misses to MyTerrainGenerator and results come back through
        // IntegratedServer::PumpChunkPipeline). The pool now only does disk
        // loads, chunk conversions and saves, so it is a CPU budget again.
        //
        // Only reached with 7+ logical cores (see the small-machine block at
        // the top): the floor of 4 is a number for machines that have the
        // cores to spend on it.
        allocation.serverWorldWorkers = std::clamp(allocation.serverWorldWorkers, size_t(4), size_t(6));
    }

    void ThreadAllocator::ValidateAllocation(ThreadAllocation& allocation) {
        // Ensure minimum workers per pool
        allocation.clientMeshWorkers = std::max(MIN_WORKERS_PER_POOL, allocation.clientMeshWorkers);
        allocation.serverWorldWorkers = std::max(MIN_WORKERS_PER_POOL, allocation.serverWorldWorkers);
        
        // Oversubscription check counts only the pools that actually compete for
        // CPU. Server world workers are excluded on purpose: they are in-flight
        // slots that spend ~98% of their life blocked in getChunk, so counting
        // them here fired a "performance may be impacted" warning on every launch
        // for threads that are asleep. (The terrain library's own pool is the one
        // doing that work, and it is not visible to this allocator at all.)
        size_t cpuThreads = allocation.reservedThreads + allocation.clientMeshWorkers;
        if (cpuThreads > allocation.totalCores) {
            Log::Warning("Thread allocation (%zu CPU-bound threads) exceeds available cores (%zu). Performance may be impacted.",
                        cpuThreads, allocation.totalCores);
        }
    }

} // namespace Core