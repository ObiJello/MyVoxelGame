// File: src/common/core/ThreadPriority.hpp
//
// Scheduling hints for the engine's long-lived threads.
//
// WHY THIS EXISTS: on Apple Silicon the CPU is heterogeneous — this M4 has 4
// performance cores and 6 efficiency cores — and threads created through
// std::thread carry no quality-of-service class at all. Without one the
// scheduler has no way to know the main thread is the one a human is waiting
// on, so it will happily leave it in the run queue behind terrain-generation
// workers, or park it on an efficiency core.
//
// The symptom is nasty to diagnose: the stall lands on whatever the main
// thread happened to be doing, which is usually glfwPollEvents (syscalls are
// natural preemption points). A profiler then reports ~49ms of "self time"
// inside a function that is documented not to block, because the thread was
// never running in the first place.
//
// Call SetCurrentThreadPriority once from INSIDE each thread, at the top of
// its entry point — the macOS API only ever applies to the calling thread.
#pragma once

namespace Core {

    enum class ThreadPriorityClass {
        // The frame. Nothing the engine owns should preempt this.
        Interactive,
        // Feeds the frame with a short deadline (mesh building, visibility).
        // Wants a fast core, but yields to Interactive.
        Elevated,
        // Throughput work with no frame deadline (terrain generation, chunk
        // I/O). Perfectly happy on efficiency cores. Deliberately NOT the
        // lowest class available: macOS confines background threads to
        // efficiency cores at very low priority, which makes chunk loading
        // crawl. This tier still gets performance cores when they are idle.
        Throughput,
    };

    // Applies to the CALLING thread. Best-effort: failures are ignored rather
    // than reported, because a missing scheduling hint degrades smoothness but
    // never correctness, and on Linux raising priority needs privileges we
    // should not require.
    void SetCurrentThreadPriority(ThreadPriorityClass cls);

} // namespace Core
