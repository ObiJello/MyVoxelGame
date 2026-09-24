// File: src/common/core/DeferredDispose.hpp
//
// Background destruction of large containers.
//
// WHY THIS EXISTS: leaving a world frees the client's chunk map (thousands
// of chunks, 24 sections each) and the server's chunk cache, and each
// container's clear() was 25-30 ms of pure deallocation on the main thread
// — together they were a third of the blank gap between the last world
// frame and the title panorama. Nothing waits on that memory: the
// containers are moved into a closure here and destroyed on one low-
// priority thread while the title screen is already up.
//
// Only pure destruction belongs here: what is handed over must not touch
// the render backend, a world, or any object with a thread affinity. A
// chunk's destructor is a memory release (sections, block entities, mesh
// snapshots), which is what makes the two call sites legal.
//
// Drain() waits for everything queued — process shutdown calls it so no
// disposal outlives main(), and a test measuring memory can call it.
#pragma once

#include <functional>
#include <memory>
#include <utility>

namespace Core::DeferredDispose {

    // Queues `fn` to run on the disposer thread (started on first use).
    // Runs it inline when the disposer is shut down. The callable may be
    // move-only (a lambda that captured a map of unique_ptr): it is boxed
    // once here, so std::function's copy requirement never applies to it.
    void Post(std::function<void()> fn);

    template <class F>
    void Run(F&& fn) {
        auto boxed = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        Post([boxed]() mutable { (*boxed)(); });
    }

    // Blocks until every queued disposal has run.
    void Drain();

    // Drain, then stop the thread. Later Run calls execute inline.
    void Shutdown();

    // Disposals queued and not yet finished (for the debug overlay).
    size_t Pending();

} // namespace Core::DeferredDispose
