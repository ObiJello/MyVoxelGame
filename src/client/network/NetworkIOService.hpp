// File: src/client/network/NetworkIOService.hpp
#pragma once

#include <boost/asio.hpp>
#include <thread>
#include <memory>
#include <atomic>

namespace Client {

    // Manages a dedicated I/O thread for network operations (Minecraft-style)
    // Similar to Minecraft's Netty event loop groups
    class NetworkIOService {
    public:
        NetworkIOService();
        ~NetworkIOService();
        
        // Non-copyable, non-movable
        NetworkIOService(const NetworkIOService&) = delete;
        NetworkIOService& operator=(const NetworkIOService&) = delete;
        
        // Start the I/O thread
        void Start();
        
        // Stop the I/O thread
        void Stop();
        
        // Get the io_context for creating sockets/connections
        boost::asio::io_context& GetIOContext() { return m_ioContext; }
        
        // Check if running
        bool IsRunning() const { return m_running.load(); }
        
    private:
        // Run the I/O context in the dedicated thread
        void RunIOThread();
        
    private:
        // The I/O context that runs on the dedicated thread
        boost::asio::io_context m_ioContext;
        
        // Work guard to keep the I/O context alive when no work
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
        std::unique_ptr<WorkGuard> m_workGuard;
        
        // The dedicated I/O thread
        std::unique_ptr<std::thread> m_ioThread;
        
        // Running state
        std::atomic<bool> m_running{false};
    };
    
    // Global I/O service for the client
    extern std::unique_ptr<NetworkIOService> g_networkIOService;
    
    // Initialize the global I/O service
    void InitializeNetworkIOService();
    
    // Shutdown the global I/O service
    void ShutdownNetworkIOService();

} // namespace Client