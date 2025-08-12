# Queues and Back-Pressure

## Queue Architecture

All inter-thread communication uses bounded queues to prevent memory exhaustion and provide back-pressure when consumers fall behind producers.

### Queue Types by Purpose

#### Packet Queues (Network ↔ Main Threads)
```cpp
// Per-connection inbound packets (I/O Thread → Server Thread)
MessageQueue<NetworkPacket> inboundQueue{1024};

// Per-connection outbound packets (Server Thread → I/O Thread)  
MessageQueue<NetworkPacket> outboundQueue{512};

// Client packet reception (I/O Thread → Client Thread)
ClientToServerQueue<DecodedPacket> clientPackets{256};
```

#### Worker Result Queues (Background → Main Threads)
```cpp
// Mesh build results (Client Workers → Client Thread)
ResultQueue<MeshBuildResult> meshResults{128};

// Chunk load results (Server Workers → Server Thread)  
ResultQueue<ChunkLoadResult> chunkResults{64};
```

## Queue Bounds and Back-Pressure Rules

### Network Packet Queues

**Inbound Packet Queue (per connection)**
- **Bound**: 1024 packets max
- **Producer**: I/O thread (packet decoder)
- **Consumer**: Server thread (`connection->tick()`)
- **Back-Pressure**: Drop oldest packets when full, disconnect on repeated overflow
- **Monitoring**: Log warning when >80% full

**Outbound Packet Queue (per connection)**
- **Bound**: 512 packets max
- **Producer**: Server thread (game logic)
- **Consumer**: I/O thread (async write operations)
- **Back-Pressure**: Block server thread briefly, then disconnect slow clients
- **Monitoring**: Track bytes queued, not just packet count

### Worker Result Queues

**Mesh Build Results**
- **Bound**: 128 results max
- **Producer**: Client worker pool (2-4 threads)
- **Consumer**: Client render thread
- **Back-Pressure**: Workers block when queue full (natural flow control)
- **Monitoring**: Measure time between build completion and GPU upload

**Chunk Load Results**
- **Bound**: 64 results max
- **Producer**: Server worker pool (2-4 threads)
- **Consumer**: Server thread
- **Back-Pressure**: Workers block when queue full
- **Monitoring**: Track load time vs. processing time

## Queue Implementation Details

### MessageQueue Template (`src/common/network/MessageQueue.hpp`)

```cpp
template<typename T>
class MessageQueue {
    static constexpr size_t DEFAULT_MAX_SIZE = 1024;
    
    // Non-blocking operations
    bool try_push(T&& message);  // Returns false if full
    bool try_pop(T& message);    // Returns false if empty
    
    // Batch operations for efficiency  
    std::vector<T> DrainAll();   // Get all messages atomically
    
    // Statistics
    size_t GetDroppedCount() const;  // Track back-pressure events
    size_t Size() const;             // Current queue depth
};
```

### Connection-Specific Queues

Each `ServerConnection` maintains separate inbound/outbound queues:

```cpp
class ServerConnection {
private:
    MessageQueue<DecodedPacket> m_inboundPackets{1024};
    MessageQueue<EncodedPacket> m_outboundPackets{512};
    
public:
    // Called by I/O thread
    bool enqueueInbound(DecodedPacket packet);
    
    // Called by server thread  
    void tick(); // Drains m_inboundPackets
    
    // Called by I/O thread
    void processOutbound(); // Drains m_outboundPackets
};
```

## Back-Pressure Handling Strategies

### Network Overload Response
1. **Warning Level** (Queue >80% full): Log performance warning
2. **Throttle Level** (Queue 95% full): Pause packet processing briefly
3. **Disconnect Level** (Queue 100% full): Close connection to prevent memory exhaustion

### Worker Overload Response
1. **Natural Blocking** (Queue full): Workers pause until space available
2. **Priority Adjustment**: Reduce priority of low-importance work
3. **Load Shedding**: Cancel distant/outdated work requests

### Queue Monitoring Metrics

**Per-Queue Statistics:**
- Current depth (number of items)
- Peak depth (high water mark)
- Drop count (back-pressure events)
- Processing rate (items/second)

**Integration with Logging:**
```cpp
// Log every 10 seconds in server tick
if (tickCount % 200 == 0) {
    Log::Info("Queue depths: inbound=%zu, outbound=%zu, meshResults=%zu", 
              inboundQueue.Size(), outboundQueue.Size(), meshResults.Size());
}
```

## Queue Bounds by Component

| Queue Type | Component | Bound | Producer | Consumer | Back-Pressure |
|------------|-----------|-------|----------|----------|---------------|
| Inbound Packets | ServerConnection | 1024 | I/O Thread | Server Thread | Drop + Disconnect |
| Outbound Packets | ServerConnection | 512 | Server Thread | I/O Thread | Brief Block + Disconnect |
| Mesh Results | ClientMeshManager | 128 | Client Workers | Client Thread | Worker Block |
| Chunk Results | ChunkProvider | 64 | Server Workers | Server Thread | Worker Block |
| Command Queue | JobSystem | 256 | Main Threads | Worker Threads | Producer Block |

## Queue Lifecycle Management

### Initialization
- All queues created during system startup with fixed bounds
- Worker threads started after queues are ready
- Connection queues created per-client during handshake

### Shutdown
- Producers stop enqueuing new items
- Consumers drain remaining items during shutdown
- Queues cleared and freed during system cleanup

### Memory Overhead
- Each queue pre-allocates space for maximum items
- Total memory usage predictable and bounded
- No dynamic allocation during steady-state operation