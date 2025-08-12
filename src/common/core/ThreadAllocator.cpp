// File: src/common/core/ThreadAllocator.cpp
#include "ThreadAllocator.hpp"
#include "Log.hpp"
#include <sstream>

namespace Core {

    std::string ThreadAllocation::ToString() const {
        std::stringstream ss;
        ss << "Thread Allocation: "
           << "Total Cores=" << totalCores
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
        
        Log::Info("Thread Allocation: %zu cores detected, %zu reserved, %zu available for workers",
                  allocation.totalCores, allocation.reservedThreads, allocation.availableWorkers);
        Log::Info("  Client Mesh Workers: %zu", allocation.clientMeshWorkers);
        Log::Info("  Server World Workers: %zu", allocation.serverWorldWorkers);
        
        return allocation;
    }

    ThreadAllocation ThreadAllocator::GetOptimalAllocation(size_t manualClientWorkers, 
                                                           size_t manualServerWorkers) {
        ThreadAllocation allocation{};
        
        allocation.totalCores = GetPhysicalCoreCount();
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
        size_t cores = std::thread::hardware_concurrency();
        
        if (cores == 0) {
            Log::Warning("Failed to detect CPU cores, using fallback value of %zu", DEFAULT_FALLBACK_CORES);
            return DEFAULT_FALLBACK_CORES;
        }
        
        return cores;
    }

    bool ThreadAllocator::HasSufficientCores() {
        size_t cores = GetPhysicalCoreCount();
        // We need at least reserved threads + 2 workers (1 per pool)
        return cores >= (TOTAL_RESERVED + 2);
    }

    void ThreadAllocator::DistributeWorkers(ThreadAllocation& allocation) {
        // Strategy: Split available workers, with slight preference to server for chunk generation
        if (allocation.availableWorkers <= 2) {
            // Minimum allocation
            allocation.clientMeshWorkers = 1;
            allocation.serverWorldWorkers = std::max(size_t(1), allocation.availableWorkers - 1);
        } else if (allocation.availableWorkers <= 4) {
            // Small systems: equal split
            allocation.clientMeshWorkers = allocation.availableWorkers / 2;
            allocation.serverWorldWorkers = allocation.availableWorkers - allocation.clientMeshWorkers;
        } else if (allocation.availableWorkers <= 8) {
            // Medium systems: slight preference to server
            allocation.clientMeshWorkers = (allocation.availableWorkers * 2) / 5; // 40%
            allocation.serverWorldWorkers = allocation.availableWorkers - allocation.clientMeshWorkers;
        } else {
            // Large systems: cap client workers at 4, rest to server
            allocation.clientMeshWorkers = std::min(size_t(4), allocation.availableWorkers / 3);
            allocation.serverWorldWorkers = allocation.availableWorkers - allocation.clientMeshWorkers;
        }
    }

    void ThreadAllocator::ValidateAllocation(ThreadAllocation& allocation) {
        // Ensure minimum workers per pool
        allocation.clientMeshWorkers = std::max(MIN_WORKERS_PER_POOL, allocation.clientMeshWorkers);
        allocation.serverWorldWorkers = std::max(MIN_WORKERS_PER_POOL, allocation.serverWorldWorkers);
        
        // Warn if we're oversubscribing
        size_t totalThreads = allocation.reservedThreads + allocation.clientMeshWorkers + allocation.serverWorldWorkers;
        if (totalThreads > allocation.totalCores) {
            Log::Warning("Thread allocation (%zu threads) exceeds available cores (%zu). Performance may be impacted.",
                        totalThreads, allocation.totalCores);
        }
    }

} // namespace Core