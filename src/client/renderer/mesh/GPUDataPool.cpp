// File: src/client/renderer/mesh/GPUDataPool.cpp
#include "GPUDataPool.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"

namespace Render {

    // Global instance
    std::unique_ptr<GPUDataPool> g_gpuDataPool = nullptr;

    GPUDataPool::GPUDataPool(size_t initialSize) {
        // Pre-allocate initial pool
        for (size_t i = 0; i < initialSize; ++i) {
            m_availablePool.push(std::make_unique<GPUSectionData>());
            m_totalAllocated++;
        }
        Log::Info("GPUDataPool initialized with %zu pre-allocated objects", initialSize);
    }

    GPUDataPool::~GPUDataPool() {
        // Clean up all GPU resources
        while (!m_availablePool.empty()) {
            clearGPUResources(m_availablePool.front().get());
            m_availablePool.pop();
        }
        
        // Clean up deferred deletions
        for (auto& deferred : m_deferredDeletions) {
            clearGPUResources(deferred.data.get());
        }
        
        Log::Info("GPUDataPool destroyed. Peak usage: %zu objects", m_peakUsage);
    }

    GPUSectionData* GPUDataPool::acquire() {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        
        GPUSectionData* result = nullptr;
        
        if (!m_availablePool.empty()) {
            // Reuse from pool
            auto data = std::move(m_availablePool.front());
            m_availablePool.pop();
            result = data.release();
        } else {
            // Pool is empty, allocate new
            result = new GPUSectionData();
            m_totalAllocated++;
            Log::Debug("GPUDataPool: Allocated new object (total: %zu)", m_totalAllocated);
        }
        
        // Update peak usage statistic
        size_t currentUsage = m_totalAllocated - m_availablePool.size();
        if (currentUsage > m_peakUsage) {
            m_peakUsage = currentUsage;
        }
        
        // Reset the object to clean state
        *result = GPUSectionData();
        
        return result;
    }

    void GPUDataPool::release(GPUSectionData* data) {
        if (!data) return;
        
        std::lock_guard<std::mutex> lock(m_poolMutex);
        
        // Destroy backend resources and reset counts
        data->DestroyAllResources(g_renderBackend.get());
        data->needsUpload = false;
        
        // Return to pool
        m_availablePool.push(std::unique_ptr<GPUSectionData>(data));
    }

    void GPUDataPool::scheduleDeferredDelete(GPUSectionData* data, uint64_t deleteAfterFrame) {
        if (!data) return;
        
        // Note: This should only be called from the main thread, so no lock needed
        DeferredDelete deferred;
        deferred.data = std::unique_ptr<GPUSectionData>(data);
        deferred.deleteAfterFrame = deleteAfterFrame;
        m_deferredDeletions.push_back(std::move(deferred));
    }

    void GPUDataPool::processDeferredDeletions(uint64_t currentFrame) {
        // Process from the back to avoid invalidating iterators
        for (auto it = m_deferredDeletions.begin(); it != m_deferredDeletions.end(); ) {
            if (it->deleteAfterFrame <= currentFrame) {
                // Return to pool or delete
                release(it->data.release());
                it = m_deferredDeletions.erase(it);
            } else {
                ++it;
            }
        }
    }

    void GPUDataPool::clearGPUResources(GPUSectionData* data) {
        if (!data) return;
        data->DestroyAllResources(g_renderBackend.get());
    }

    // Global initialization/shutdown functions
    bool InitializeGPUDataPool(size_t initialSize) {
        if (g_gpuDataPool) {
            Log::Warning("GPUDataPool already initialized");
            return true;
        }
        
        g_gpuDataPool = std::make_unique<GPUDataPool>(initialSize);
        return true;
    }

    void ShutdownGPUDataPool() {
        if (g_gpuDataPool) {
            g_gpuDataPool.reset();
            Log::Info("GPUDataPool shutdown complete");
        }
    }

} // namespace Render