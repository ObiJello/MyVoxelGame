// File: src/client/renderer/mesh/GPUDataPool.hpp
#pragma once

#include "SectionMesh.hpp"
#include <queue>
#include <mutex>
#include <memory>
#include <vector>
#include <chrono>

namespace Render {

    // Manages a pool of GPUSectionData objects for efficient recycling
    // Avoids constant allocation/deallocation and OpenGL object creation/destruction
    class GPUDataPool {
    public:
        GPUDataPool(size_t initialSize = 256);
        ~GPUDataPool();

        // Acquire a GPUSectionData from the pool (or create new if pool is empty)
        GPUSectionData* acquire();

        // Release a GPUSectionData back to the pool for reuse
        void release(GPUSectionData* data);

        // Schedule data for deferred deletion (deleted after N frames to avoid use-after-free)
        void scheduleDeferredDelete(GPUSectionData* data, uint64_t deleteAfterFrame);

        // Process deferred deletions for the current frame
        void processDeferredDeletions(uint64_t currentFrame);

        // Get pool statistics
        size_t getPoolSize() const { return m_availablePool.size(); }
        size_t getInUseCount() const { return m_totalAllocated - m_availablePool.size(); }
        size_t getDeferredCount() const { return m_deferredDeletions.size(); }

    private:
        // Clear all OpenGL resources from a GPUSectionData
        void clearGPUResources(GPUSectionData* data);

        // Pool of available GPUSectionData objects
        std::queue<std::unique_ptr<GPUSectionData>> m_availablePool;
        
        // Deferred deletion queue (pair of data and frame to delete after)
        struct DeferredDelete {
            std::unique_ptr<GPUSectionData> data;
            uint64_t deleteAfterFrame;
        };
        std::vector<DeferredDelete> m_deferredDeletions;

        // Thread safety (only for acquire/release, deferred deletions are main thread only)
        mutable std::mutex m_poolMutex;

        // Statistics
        size_t m_totalAllocated = 0;
        size_t m_peakUsage = 0;
    };

    // Global GPU data pool instance
    extern std::unique_ptr<GPUDataPool> g_gpuDataPool;
    
    // Initialize/shutdown functions
    bool InitializeGPUDataPool(size_t initialSize = 256);
    void ShutdownGPUDataPool();

} // namespace Render