ADR-0003: Separate I/O Threads with No Polling

Status

Accepted

Context

The server architecture requires careful coordination between network I/O and game simulation. Two critical requirements must be balanced:

1. Consistent Tick Timing: Server must maintain 20 TPS (50ms per tick) regardless of network conditions
2. Responsive Networking: Client connections must be serviced promptly without blocking game logic

Traditional approaches often compromise one of these requirements:
- Polling in tick loop: io_context.poll() in main thread blocks during heavy network traffic
- Blocking I/O: Synchronous socket operations can pause server ticks indefinitely
- Mixed threading: Some I/O on main thread creates unpredictable timing

We need a clean separation that mirrors Minecraft Java Edition's Netty threading model.

Decision

We will use dedicated I/O threads that run boost::asio::io_context::run() continuously, with zero polling in the server tick loop. All network operations are asynchronous and handled exclusively on
I/O threads.

Rationale

Minecraft Java Edition Pattern

Minecraft uses Netty's EventLoopGroup pattern:
- Server Thread: Runs at 20 TPS, never performs I/O operations directly
- Netty I/O Threads: Dedicated threads running EventLoop.run() continuously
- Channel Handlers: Process packets on I/O threads, queue results for server thread
- Work Isolation: Server thread drains packet queues, never calls selector.select()

Our Boost.Asio equivalent:

    // I/O Thread: Runs continuously
    m_networkThread = std::make_unique<std::thread>([this]() {
        m_ioContext->run(); // Equivalent to Netty's EventLoop.run()
    });
    
    // Server Thread: Fixed 20 TPS, never calls poll()
    void ServerTick() {
        // Drain packets (queued by I/O thread)
        for (auto& conn : connections) {
            conn->tick(); // Process queued packets only
        }
    
        // Never calls: m_ioContext->poll() or any blocking I/O
        RunWorldSimulation();
    }

Technical Benefits

Predictable Tick Timing
- Server tick duration unaffected by network latency or connection count
- No risk of blocking on slow/unresponsive clients
- Consistent frame time for physics and game simulation
- Eliminates jitter caused by intermittent I/O operations

Network Responsiveness
- I/O thread responds to network events immediately
- No waiting for next server tick to process incoming data
- Outbound packets sent as soon as queued (no batching delays)
- Connection timeouts and keep-alives handled promptly

Scalability Properties
- Adding more connections doesn't impact server tick performance
- I/O thread count can scale independently of game logic
- Network-heavy operations (compression, encryption) don't affect simulation

Implementation Architecture

I/O Thread Lifecycle

    // In IntegratedServer::Start()
    bool IntegratedServer::Start() {
        // Create io_context for network operations
        m_ioContext = std::make_unique<boost::asio::io_context>();
    
        // Create work guard to prevent io_context from exiting
        m_ioWorkGuard = std::make_unique<WorkGuard>(
          boost::asio::make_work_guard(*m_ioContext)
        );
    
        // Start dedicated I/O thread
        m_networkThread = std::make_unique<std::thread>([this]() {
            Log::Info("Server network I/O thread started");
            try {
                 m_ioContext->run(); // Runs until stop() called
            } catch (const std::exception& e) {
                Log::Error("Server network I/O thread exception: %s", e.what());
            }
        });
    
        // Start server tick thread separately
        m_serverThread = std::make_unique<std::thread>([this]() {
             ServerLoop(); // 20 TPS loop, never calls I/O
        });
        
        // CRITICAL: For singleplayer, ensure io_context runs on dedicated thread
        // even when using TCP localhost connection (not polled from tick loop)
    }

Work Guard Pattern

    // Work guard keeps io_context alive even with no pending operations
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    m_ioWorkGuard = std::make_unique<WorkGuard>(boost::asio::make_work_guard(*m_ioContext));
    
    // During shutdown:
    m_ioWorkGuard.reset(); // Allow io_context to exit
    m_ioContext->stop();   // Stop run() loop
    if (m_networkThread->joinable()) {
        m_networkThread->join(); // Wait for clean exit
    }

Packet Queue Bridge
    
    // I/O Thread: Decode and enqueue packets
    void ServerConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
        // This runs on I/O thread via async_read completion
        auto packet = DecodePacket(packetId, payload);
        if (packet) {
            m_inboundPackets.try_push(std::move(packet)); // Thread-safe queue
        }
    }
    
    // Server Thread: Drain and process packets  
    void ServerConnection::tick() {
        // This runs on server thread during ServerTick()
        auto packets = m_inboundPackets.DrainAll(); // Drain entire queue atomically
    
        for (const auto& packet : packets) {
            packet->apply(*m_listener); // Process game logic
        }
    }

Async Operation Chain
    
    // All I/O operations are asynchronous chains
    void ServerConnection::StartRead() {
        // Read packet length header
        boost::asio::async_read(m_socket, boost::asio::buffer(&m_packetLength, sizeof(m_packetLength)),
        boost::asio::bind_executor(m_strand, [this](boost::system::error_code ec, size_t) {
            if (!ec) {
                // Read packet payload
                m_packetBuffer.resize(m_packetLength);
                boost::asio::async_read(m_socket, boost::asio::buffer(m_packetBuffer),
                       boost::asio::bind_executor(m_strand, [this](boost::system::error_code ec, size_t) {
                    if (!ec) {
                        ProcessPacket(m_packetBuffer);
                        StartRead(); // Continue reading next packet
                    }
                }));
            }
        }));
    }

Threading Guarantees

I/O Thread Responsibilities

- Socket Operations: All async_read, async_write, async_accept
- Packet Decode: Parse VarInt headers, decompress, deserialize to typed packets
- Connection Management: Handle connects, disconnects, timeouts
- Protocol State: Manage handshake → login → play transitions
- Compression Pipeline: Install/remove compression when SetCompression sent

Server Thread Responsibilities

- Game Logic: World simulation, entity updates, physics
- Packet Processing: Apply decoded packets to game state
- Packet Generation: Create outbound packets based on game events
- Player Management: Track player positions, chunk loading, inventory

Prohibited Operations

Server Thread NEVER calls:
- io_context::poll() or io_context::run()
- Any synchronous socket operations
- Direct socket reads/writes
- Blocking network calls

I/O Thread NEVER calls:
- World modification methods
- Direct game state changes
- OpenGL operations
- File I/O (except for network-related operations)

**Critical Threading Responsibility**: The I/O thread should never touch world state; it only enqueues to per-connection queues that the server thread drains.

Performance Characteristics

Tick Time Predictability

    // Server tick timing unaffected by network conditions
    void ServerTick() {
        auto startTime = std::chrono::steady_clock::now();
    
        // Process packets (bounded by queue size, not network speed)
        ProcessClientPackets(); // ~1-5ms typical
    
        // Run world simulation (deterministic timing)
        m_world->WorldLoop(deltaTime); // ~10-20ms typical
    
        // Total tick time: 15-25ms typical, never blocked by network
        auto endTime = std::chrono::steady_clock::now();
        auto tickTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    
        // Should consistently be <30ms
        if (tickTime > 40.0f) {
            Log::Warning("Slow server tick: %.2fms", tickTime);
        }
    }

Network Latency Independence

- Client packets processed within 1-2ms of arrival (I/O thread response)
- Server tick timing unaffected by client network conditions
- Slow clients don't impact fast clients or server performance
- Connection issues isolated to individual connections

Shutdown Sequence

Graceful Shutdown Order

    void IntegratedServer::Stop() {
        // 1. Signal both threads to stop
        m_shouldStop.store(true);
    
        // 2. Wait for server thread to complete current tick
        if (m_serverThread && m_serverThread->joinable()) {
            m_serverThread->join(); // Clean exit from ServerLoop()
        }
    
        // 3. Stop I/O operations
        m_ioWorkGuard.reset();   // Allow io_context to exit
        m_ioContext->stop();     // Stop async operations
    
        // 4. Wait for I/O thread to finish
        if (m_networkThread && m_networkThread->joinable()) {
            m_networkThread->join(); // Clean exit from run()
        }
    
        // 5. Now safe to destroy network resources
        m_networkServer.reset();
        m_ioContext.reset();
    }

Consequences

Positive

- Timing Consistency: Server ticks maintain precise 20 TPS regardless of network load
- Network Responsiveness: I/O operations handled immediately without waiting for ticks
- Scalability: More connections don't slow down server simulation
- Error Isolation: Network issues don't crash or block server thread
- Architecture Clarity: Clean separation of concerns between networking and game logic

Negative

- Complexity: Two-thread coordination more complex than single-threaded polling
- Memory Usage: Packet queues consume more memory than immediate processing
- Latency: Small additional latency from queue buffering (typically <1ms)
- Debug Complexity: Issues may occur on different threads, harder to trace

Neutral

- Resource Usage: One additional thread per server instance
- Queue Management: Bounded queues prevent runaway memory usage
- Thread Safety: Well-defined ownership model eliminates most synchronization issues

Alternatives Considered

Polling in Server Tick

    void ServerTick() {
        m_ioContext->poll(); // Process network events
        RunWorldSimulation(); // Game logic
    }
Rejected: Server tick time becomes unpredictable, affected by network conditions

Blocking I/O on Server Thread

    void ProcessClient(tcp::socket& socket) {
        socket.read_some(buffer); // Blocks until data available
        ProcessPacket(buffer);
    }
Rejected: One slow client can block entire server, no concurrency

Mixed Threading (Some I/O on Server Thread)

    void ServerTick() {
        ProcessUrgentPackets();        // On server thread
        m_ioContext->poll_one();       // Limited I/O processing
        RunWorldSimulation();
    }
Rejected: Still introduces timing variability, complex to reason about

The separate I/O thread approach provides the best balance of performance, predictability, and architectural clarity while following proven patterns from Minecraft's implementation.
