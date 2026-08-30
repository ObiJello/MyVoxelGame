// File: src/common/core/TickParallel.hpp
//
// A fork-join parallel_for for work that happens INSIDE a server tick.
//
// Why not JobSystem::g_ThreadPool: that pool is a fire-and-forget queue
// (Enqueue only, no join), shared with chunk meshing and world I/O whose jobs
// can run for milliseconds. A tick-bound fan-out has to finish before the tick
// can continue, so queueing behind a mesh build would make the tick WAIT on
// unrelated work. This pool exists only for the fork-join shape and its workers
// do nothing else.
//
// The calling thread participates rather than blocking, so an N-worker pool
// gives N+1 runners and a one-chunk workload costs nothing but the claim.
//
// SCOPE: this is a data-parallel map over an index range. The body must be
// PURE with respect to shared state — read-only against the world, writing only
// to per-index output slots. It is not a general task system and there is no
// nesting: calling ParallelFor from inside a ParallelFor body runs the inner
// range serially on the calling worker.
#pragma once

#include <cstddef>
#include <functional>

namespace Core {

    // Run fn(i) for i in [0, count). Returns when every index has completed.
    // `grain` is the minimum number of indices one thread claims at a time;
    // it should be at least a cache line's worth of whatever fn writes, so
    // neighbouring workers do not share a dirty line.
    //
    // An exception escaping fn calls std::terminate. That is ENFORCED by the
    // noexcept below rather than merely documented: unwinding out of a running
    // batch would destroy the batch and the caller's std::function while
    // workers are still calling through them.
    void ParallelFor(size_t count, size_t grain,
                     const std::function<void(size_t)>& fn) noexcept;

    // Number of runners ParallelFor will use, including the calling thread.
    // 1 means everything runs inline.
    size_t ParallelWidth();

    // Idempotent. Safe to call before main's threads exist; ParallelFor calls
    // it itself on first use.
    void InitTickParallel();
    void ShutdownTickParallel();

} // namespace Core
