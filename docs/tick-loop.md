Server Tick Loop

20 TPS Fixed Step Timing

The server maintains a precise 20 ticks per second (50ms per tick) using fixed-step timing:

    // In IntegratedServer::ServerLoop() (src/server/IntegratedServer.cpp:236-264)
    void ServerLoop() {
        m_lastTickTime = std::chrono::steady_clock::now();

        while (!m_shouldStop.load()) {
            auto tickStart = std::chrono::steady_clock::now();

            try {
                ServerTick(); // Core game logic
             }
            catch (const std::exception& e) {
                Log::Error("Server tick failed: %s", e.what());
            }

            // Precise timing control
            WaitForNextTick(tickStart);
        }
    }

Tick Timing Implementation

    // In IntegratedServer::WaitForNextTick()
    void WaitForNextTick(std::chrono::steady_clock::time_point tickStart) {
        auto tickEnd = std::chrono::steady_clock::now();
        auto tickDuration = tickEnd - tickStart;
        auto targetDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(m_tickDuration); // 50ms

        if (tickDuration < targetDuration) {
            // Sleep until next tick
            auto sleepTime = targetDuration - tickDuration;
            std::this_thread::sleep_for(sleepTime);
        } else if (tickDuration > targetDuration) {
            // Handle catch-up ticks (like Minecraft's "Can't keep up!")
            auto deficit = tickDuration - targetDuration;
            int catchupTicks = std::min(5, static_cast<int>(deficit / targetDuration)); // Cap at 5 extra ticks
            
            if (catchupTicks > 0) {
                Log::Warning("Can't keep up! Running %d catch-up ticks", catchupTicks);
                for (int i = 0; i < catchupTicks; ++i) {
                    try {
                        ServerTick(); // Additional ticks to catch up
                    } catch (const std::exception& e) {
                        Log::Error("Catch-up tick failed: %s", e.what());
                        break; // Stop catch-up if tick fails
                    }
                }
            }
            
            float tickTimeMs = std::chrono::duration<float, std::milli>(tickDuration).count();
            Log::Warning("Server tick took %.2fms (target: 50ms)", tickTimeMs);
        }
    }

Exact Tick Order

Phase 1: Connection Drain (CRITICAL FIRST)

    // In IntegratedServer::ServerTick() (lines 276-282)
    if (m_networkServer) {
        auto connections = m_networkServer->GetConnections();
        for (auto& conn : connections) {
            conn->tick();  // Drain incoming packets and apply to listeners
        }
    }

What happens in ServerConnection::tick():
1. Drain all packets from connection's inbound queue
2. Decode and validate each packet
3. Route packets to current protocol state listener
4. Apply packet effects (player movement, block actions, etc.)
5. Update connection activity timestamps

Why First: All client input must be processed before world simulation to ensure consistency

Phase 2: World Simulation

    // In IntegratedServer::ServerTick() (lines 284-287)
    if (m_world) {
        m_world->WorldLoop(deltaTime);
    }

World simulation includes:
- Process queued block changes from Phase 1
- Update chunk loading around player positions
- Run entity physics and AI
- Process scheduled world events
- Generate new chunks via ServerWorkerPool

Phase 3: Gather Diffs and Enqueue S2C Packets (EXPLICIT SEND PHASE)

    // In IntegratedServer::ServerTick() - after world simulation
    GatherAndEnqueueOutboundPackets();

Send phase operations:
- Serialize block/multi-block changes from world diffs
- Package light updates for modified sections
- Generate ChunkDataS2C packets for newly loaded chunks
- Create UnloadChunkS2C packets for chunks leaving view distance
- Enqueue all S2C packets for I/O thread transmission

Phase 4: Statistics and Maintenance

    // In IntegratedServer::ServerTick() (lines 289-296)  
    m_stats.ticksProcessed.fetch_add(1);
    
    // Periodic logging (every 10 seconds)
    static uint64_t logCounter = 0;
    if (++logCounter % (m_config.tickRate * 10) == 0) {
        LogServerState();
    }

Maintenance operations:
- Update performance statistics
- Check for player timeouts
- Clean up expired resources
- Log server health metrics

No io_context::poll() Policy

CRITICAL: The server thread never calls io_context::poll() or any network I/O operations directly.

Why This Matters

- Thread Separation: Network I/O handled exclusively by dedicated I/O thread
- Predictable Timing: Server tick timing unaffected by network latency or blocking
- Scalability: Multiple connections don't impact server tick performance
- Error Isolation: Network errors don't crash server thread

I/O Thread Responsibility

    // In IntegratedServer::Start() - I/O thread creation (lines 92-101)
    m_networkThread = std::make_unique<std::thread>([this]() {
        Log::Info("Server network I/O thread started");
        try {
            m_ioContext->run(); // This runs continuously
            Log::Info("Server network I/O thread exiting normally");
        } catch (const std::exception& e) {
            Log::Error("Server network I/O thread exception: %s", e.what());
        }
    });

Work Guard Pattern

    // Keep io_context alive even when no pending work
    m_ioWorkGuard = std::make_unique<WorkGuard>(boost::asio::make_work_guard(*m_ioContext));
    
    // During shutdown:
    m_ioWorkGuard.reset(); // Allow io_context to exit
    m_ioContext->stop();   // Stop the run loop

Packet Processing Details

Inbound Packet Flow

I/O Thread: Raw Bytes → VarInt Decode → Packet Object → Connection Queue
Server Thread: conn->tick() → Drain Queue → Process Packets → Update Game State

Per-Connection Tick Processing

    // In ServerConnection::tick()
    void ServerConnection::tick() {
        // Drain all inbound packets from I/O thread
        auto packets = m_inboundPackets.DrainAll();
    
        for (const auto& packet : packets) {
            try {
                // Apply packet via current protocol listener
                m_listener->apply(packet);

                // Update activity tracking
                UpdateActivity();
            } catch (const std::exception& e) {
                Log::Warning("Packet processing failed: %s", e.what());
                // Consider disconnecting on repeated failures
            } 
        }

        // Check for keep-alive timeout
        if (IsTimedOut()) {
              SendDisconnect("Timed out");
            Close();
        }
    }

Outbound Packet Flow

Server Thread: Create Packet → Serialize → Queue for I/O Thread
I/O Thread: Drain Queue → Compress → Send via Socket

Performance Budgets and Monitoring

Per-Tick Time Budgets

- Connection Drain: 5-10ms max (scales with player count)
- World Simulation: 20-30ms max (depends on active chunks)
- Maintenance: 1-2ms max
- Total Tick: 50ms hard limit, 30ms target for stability

Performance Statistics

    // In IntegratedServer::UpdateStatistics()
    struct ServerStats {
        std::atomic<uint64_t> ticksProcessed{0};
        std::atomic<float> averageTickTime{0.0f};    // Exponential moving average
        std::atomic<float> averageTPS{20.0f};        // Actual measured TPS
        std::atomic<uint64_t> packetsReceived{0};    // All inbound packets
        std::atomic<uint64_t> packetsSent{0};        // All outbound packets
    };
    
    // Update every tick
    void UpdateStatistics(float tickTime) {
        // Moving average of tick times
        float currentAvg = m_stats.averageTickTime.load();
        float newAvg = currentAvg * 0.9f + tickTime * 0.1f;
        m_stats.averageTickTime.store(newAvg);

        // Actual TPS calculation  
        if (tickTime > 0.0f) {
            float tps = 1000.0f / tickTime;
            float currentTPS = m_stats.averageTPS.load();
            float newTPS = currentTPS * 0.9f + tps * 0.1f;
            m_stats.averageTPS.store(newTPS);
        }
    }

Health Monitoring

    // In IntegratedServer::LogServerState() (called every 10 seconds)
    void LogServerState() const {
        Log::Info("Server: TPS=%.1f, TickTime=%.2fms, LoadedChunks=%zu, PendingLoads=%zu",
            m_stats.averageTPS.load(),
            m_stats.averageTickTime.load(),
            m_playerState.loadedChunks.size(),
            m_pendingChunkLoads.size());
    }

Overload Detection and Response

    // Track consecutive slow ticks
    static int slowTickCount = 0;
    if (tickTime > 60.0f) { // 20% over budget
        slowTickCount++;
        if (slowTickCount > 10) {
            Log::Warning("Server overloaded: %d consecutive slow ticks", slowTickCount);
            // Could implement load shedding here
        }
    } else {
        slowTickCount = 0;
    }

Shutdown Sequence

Graceful Tick Loop Exit

    // In IntegratedServer::Stop() (lines 122-133)
    m_shouldStop.store(true);
    
    // Wait for server thread to complete current tick and exit
    if (m_serverThread && m_serverThread->joinable()) {
        Log::Debug("Waiting for server thread to finish...");
        m_serverThread->join();
        Log::Debug("Server thread finished");
    }

Resource Cleanup Order

1. Signal Stop: Set m_shouldStop atomic flag
2. Complete Current Tick: Server thread finishes current ServerTick() call
3. Join Server Thread: Wait for clean exit from tick loop
4. Stop I/O Thread: Reset work guard, stop io_context, join I/O thread
5. Cleanup Resources: Close connections, free world data

The tick loop design ensures consistent, predictable server behavior with clear separation between network I/O and game simulation, enabling both single-player performance and multiplayer
scalability.