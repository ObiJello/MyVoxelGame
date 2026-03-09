// File: src/common/core/ThreadAllocator.hpp
#pragma once

#include <cstddef>
#include <thread>
#include <algorithm>
#include <string>

namespace Core {

    struct ThreadAllocation {
        size_t clientMeshWorkers;
        size_t serverWorldWorkers;
        size_t totalCores;
        size_t reservedThreads;
        size_t availableWorkers;
        
        std::string ToString() const;
    };

    class ThreadAllocator {
    public:
        static constexpr size_t MIN_WORKERS_PER_POOL = 1;
        static constexpr size_t DEFAULT_FALLBACK_CORES = 4;
        
        enum class ReservedThread {
            RENDER = 1,          // Client render thread
            SERVER = 1,          // Server tick thread
            IO = 1,              // Network I/O thread (shared client+server, mostly idle)
        };

        static constexpr size_t TOTAL_RESERVED =
            static_cast<size_t>(ReservedThread::RENDER) +
            static_cast<size_t>(ReservedThread::SERVER) +
            static_cast<size_t>(ReservedThread::IO);
        
        // Get optimal thread allocation based on system capabilities
        static ThreadAllocation GetOptimalAllocation();
        
        // Get optimal allocation with manual override
        static ThreadAllocation GetOptimalAllocation(size_t manualClientWorkers, 
                                                    size_t manualServerWorkers);
        
        // Get the number of physical CPU cores
        static size_t GetPhysicalCoreCount();
        
        // Check if we have enough cores for the game to run well
        static bool HasSufficientCores();
        
    private:
        // Calculate worker thread distribution
        static void DistributeWorkers(ThreadAllocation& allocation);
        
        // Validate and adjust allocation to meet minimum requirements
        static void ValidateAllocation(ThreadAllocation& allocation);
    };

} // namespace Core