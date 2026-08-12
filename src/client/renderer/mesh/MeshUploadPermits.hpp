// File: src/client/renderer/mesh/MeshUploadPermits.hpp
//
// Port of net/minecraft/client/renderer/SectionBufferBuilderPool.
//
// MC bounds its chunk pipeline with a fixed pool of scratch buffer packs, NOT
// with a time or byte budget. SectionRenderDispatcher.runTask (:74) refuses to
// start a compile unless a pack is free:
//
//     if (!this.closed && !this.bufferPool.isEmpty()) {
//         RenderSection.CompileTask task = this.compileQueue.poll(...);
//         SectionBufferBuilderPack buffer = this.bufferPool.acquire();
//
// and the pack is only returned once that section's UPLOAD has completed on
// the render thread (:84-89). Because one permit spans compile AND upload, a
// slow upload stage stops new compiles automatically — the queue between them
// physically cannot grow. That is why uploadAllPendingUploads (:104) can drain
// its entire queue every frame with no budget at all: the pool already bounded
// what was allowed to be in it.
//
// The distinction that matters: budgeting the drain by CPU milliseconds does
// not work, because glBufferSubData returns as soon as the driver has staged
// the copy. The transfer is paid later, at the swap. A permit pool sidesteps
// the whole estimation problem — the resource IS the budget.
#pragma once

#include <atomic>
#include <cstddef>

namespace Render {

    class MeshUploadPermits {
    public:
        // capacity mirrors MC's Minecraft.java:550, which sizes the pool from
        // Runtime.getRuntime().availableProcessors().
        void Initialize(size_t capacity) {
            if (capacity == 0) capacity = 1;
            m_capacity.store(capacity, std::memory_order_release);
            m_available.store(capacity, std::memory_order_release);
        }

        // False means the pipeline is full. The caller must NOT submit — leave
        // the section dirty and let the next schedule pass retry it.
        bool TryAcquire() {
            size_t cur = m_available.load(std::memory_order_relaxed);
            while (cur > 0) {
                if (m_available.compare_exchange_weak(cur, cur - 1,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    return true;
                }
                // cur is refreshed by compare_exchange_weak on failure.
            }
            return false;
        }

        // MUST run exactly once for every successful TryAcquire, on EVERY path
        // the job can take — uploaded, dropped by a full queue, cancelled, or
        // discarded with the job queue. A leaked permit permanently shrinks the
        // pipeline; leak enough and meshing stops for good, so the release
        // sites use RAII wherever the path can branch.
        void Release() {
            const size_t cap = m_capacity.load(std::memory_order_acquire);
            size_t prev = m_available.fetch_add(1, std::memory_order_acq_rel);
            if (prev + 1 > cap) {
                // Over-release is an accounting bug on our side. Clamp so a
                // double-release can't inflate the pool past its capacity and
                // silently disable the backpressure entirely.
                m_available.store(cap, std::memory_order_release);
            }
        }

        // No separate reset entry point: Initialize() already restores the full
        // count and runs once per session inside PlatformMain's session loop,
        // so quit-to-title → rejoin starts from a clean pool even if a session
        // ended with jobs in flight.

        size_t Available() const { return m_available.load(std::memory_order_relaxed); }
        size_t Capacity()  const { return m_capacity.load(std::memory_order_relaxed); }

        // Compile + upload stages combined — MC's "pC + pU" debug readout.
        size_t InFlight() const {
            const size_t cap = Capacity();
            const size_t avail = Available();
            return avail >= cap ? 0 : cap - avail;
        }

    private:
        std::atomic<size_t> m_available{0};
        std::atomic<size_t> m_capacity{0};
    };

    // Process-wide instance. It lives in its own header rather than on
    // ClientMeshManager because ClientChunkManager needs it too, and
    // ClientMeshManager.hpp already includes ClientChunkManager.hpp.
    inline MeshUploadPermits& GetMeshUploadPermits() {
        static MeshUploadPermits s_permits;
        return s_permits;
    }

} // namespace Render
