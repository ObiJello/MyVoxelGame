// File: src/client/network/NetworkIOService.cpp
#include "NetworkIOService.hpp"
#include "common/core/Log.hpp"
#include <chrono>

namespace Client {

    // Global I/O service instance
    std::unique_ptr<NetworkIOService> g_networkIOService;
    
    NetworkIOService::NetworkIOService() 
        : m_ioContext()
        , m_workGuard(nullptr)
        , m_ioThread(nullptr)
        , m_running(false) {
    }
    
    NetworkIOService::~NetworkIOService() {
        Stop();
    }
    
    void NetworkIOService::Start() {
        if (m_running.exchange(true)) {
            Log::Warning("NetworkIOService: Already running");
            return;
        }
        
        // Create work guard to keep io_context alive
        m_workGuard = std::make_unique<WorkGuard>(net::make_work_guard(m_ioContext));
        
        // Start the I/O thread
        m_ioThread = std::make_unique<std::thread>([this]() {
            RunIOThread();
        });
        
        Log::Info("NetworkIOService: Started I/O thread");
    }
    
    void NetworkIOService::Stop() {
        if (!m_running.exchange(false)) {
            return;
        }
        
        Log::Info("NetworkIOService: Stopping I/O thread...");
        
        // Reset work guard to allow io_context to exit
        m_workGuard.reset();
        
        // Stop the io_context
        m_ioContext.stop();
        
        // Wait for the I/O thread to finish
        if (m_ioThread && m_ioThread->joinable()) {
            m_ioThread->join();
        }
        
        // Clean up
        m_ioThread.reset();
        
        Log::Info("NetworkIOService: I/O thread stopped");
    }
    
    void NetworkIOService::RunIOThread() {
        Log::Info("NetworkIOService: I/O thread started (tid: %zu)", 
                  std::hash<std::thread::id>{}(std::this_thread::get_id()));
        
        try {
            // Run the I/O context event loop
            size_t handlers_executed = m_ioContext.run();
            Log::Info("NetworkIOService: I/O thread exiting, executed %zu handlers", handlers_executed);
        } catch (const std::exception& e) {
            Log::Error("NetworkIOService: Exception in I/O thread: %s", e.what());
        } catch (...) {
            Log::Error("NetworkIOService: Unknown exception in I/O thread");
        }
        
        m_running = false;
    }
    
    void InitializeNetworkIOService() {
        if (g_networkIOService) {
            Log::Warning("NetworkIOService: Already initialized");
            return;
        }
        
        g_networkIOService = std::make_unique<NetworkIOService>();
        g_networkIOService->Start();
        
        Log::Info("NetworkIOService: Initialized and started");
    }
    
    void ShutdownNetworkIOService() {
        if (!g_networkIOService) {
            return;
        }
        
        Log::Info("NetworkIOService: Shutting down...");
        g_networkIOService->Stop();
        g_networkIOService.reset();
        Log::Info("NetworkIOService: Shutdown complete");
    }

} // namespace Client