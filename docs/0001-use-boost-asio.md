ADR-0001: Use Boost.Asio for Networking

Status

Accepted

Context

MyVoxelGame requires a robust networking foundation for the integrated server architecture. The system needs to handle:
- Multiple concurrent client connections (future multiplayer)
- Asynchronous packet I/O without blocking game simulation
- Protocol state management (handshake, login, play states)
- Packet compression and framing similar to Minecraft
- Thread-safe connection management with proper resource cleanup

We evaluated several networking approaches:
1. Raw POSIX sockets: Low-level, platform-specific, requires manual async handling
2. Custom event loop: High development cost, potential for bugs in complex scenarios
3. Boost.Asio: Mature, cross-platform, proven async I/O patterns
4. Other libraries: Qt Network (heavy dependency), libevent (C-style API), asio standalone (similar to Boost.Asio)

Decision

We will use Boost.Asio as the networking foundation, specifically implementing patterns that mirror Minecraft Java Edition's Netty usage.

Rationale

Proven Architecture Pattern

Minecraft Java Edition uses Netty (Java's premier async networking library) with specific patterns:
- Dedicated I/O threads running event loops
- Per-connection channel pipelines for protocol handling
- Strand-like constructs (EventLoop affinity) for thread safety
- Separation between I/O operations and game logic threads

Boost.Asio provides direct C++ equivalents to these Netty patterns:
- io_context::run() ≡ Netty's EventLoopGroup.next().run()
- strand ≡ Netty's channel EventLoop (single-threaded execution guarantee)
- Async operation chains ≡ Netty's ChannelPipeline transformations
- Work guards ≡ Netty's EventLoop lifecycle management

Technical Advantages

Mature Async Model
- Battle-tested async I/O patterns used in production systems
- Comprehensive error handling and edge case management
- Built-in support for connection timeouts, keep-alives, and graceful shutdown

Cross-Platform Compatibility
- Single codebase works on Windows (IOCP), Linux (epoll), macOS (kqueue)
- Abstracts platform-specific networking differences
- Consistent behavior across development and deployment environments

Performance Characteristics
- Zero-copy operations where possible
- Efficient event notification mechanisms (epoll/IOCP/kqueue)
- Scalable to hundreds of concurrent connections
- Minimal context switching overhead

Thread Safety Model
- Strand concept provides lock-free single-threaded execution contexts
- Eliminates most manual synchronization around connection state
- Clear ownership model for connection resources

Implementation Strategy

I/O Thread Separation

    // Dedicated I/O thread running continuously  
    m_ioContext = std::make_unique<boost::asio::io_context>();
    m_networkThread = std::make_unique<std::thread>([this]() {
        m_ioContext->run(); // Never returns until stop() called
    });

Per-Connection Strand

    // Each ServerConnection owns a strand for thread safety
    class ServerConnection {
        boost::asio::io_context::strand m_strand;

        void async_read() {
             // All connection operations run on same strand
            boost::asio::async_read(m_socket, m_buffer,
              boost::asio::bind_executor(m_strand, [this](error_code ec, size_t bytes) {
                  // Guaranteed single-threaded execution per connection
                  handle_read(ec, bytes);
              }));
        }
    };
    
    Work Guard Pattern
    // Keep io_context alive even with no pending operations
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    m_ioWorkGuard = std::make_unique<WorkGuard>(boost::asio::make_work_guard(*m_ioContext));
    
    // Shutdown: reset work guard, then stop io_context
    m_ioWorkGuard.reset();
    m_ioContext->stop();

Consequences

Positive

- Architecture Alignment: Direct mapping from proven Minecraft Netty patterns to C++
- Reduced Development Time: No need to implement custom async I/O framework
- Better Reliability: Leverage years of Boost.Asio bug fixes and optimizations
- Future Scalability: Ready for multiplayer server with minimal changes
- Easier Debugging: Well-documented library with extensive community knowledge

Negative

- Dependency Weight: Boost.Asio adds ~2MB to binary size
- Learning Curve: Team needs to understand async programming concepts and strand model
- Template Complexity: Heavy template usage can impact compile times and error messages
- Header-Only Overhead: Boost.Asio headers add to compilation time

Neutral

- API Complexity: Boost.Asio is complex but so is any robust async networking solution
- Performance Overhead: Minimal compared to alternatives once properly configured
- Platform Dependencies: Requires Boost, but we already use other Boost components

Implementation Notes

Required Boost Components

We use Boost.Asio as header-only, requiring these Boost libraries:
- system (for error_code)
- thread (if not using std::thread)
- Headers: asio.hpp, bind_executor.hpp, strand.hpp

Integration Points

- NetworkServer uses tcp::acceptor for connection handling
- ServerConnection wraps tcp::socket with strand for thread safety
- Packet encoding/decoding happens on I/O thread, game logic on server thread
- Message queues bridge async I/O operations with synchronous game ticks

Migration Path

The Boost.Asio patterns established here create a clean foundation that could later migrate to:
- Standalone Asio (remove Boost dependency)
- Custom networking layer (if future requirements demand it)
- Different async I/O libraries (with interface compatibility layer)

This decision provides the networking foundation needed for both current single-player requirements and future multiplayer expansion, following proven architectural patterns from Minecraft's own
implementation.