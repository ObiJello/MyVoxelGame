Logging and Metrics

Key Performance Counters

Network Metrics

    struct NetworkMetrics {
    // Byte counters
    std::atomic<uint64_t> bytesReceived{0};        // Total inbound bytes
    std::atomic<uint64_t> bytesSent{0};            // Total outbound bytes
    std::atomic<uint64_t> bytesCompressed{0};      // Bytes after compression
    std::atomic<uint64_t> bytesDecompressed{0};    // Bytes after decompression

      // Packet counters
      std::atomic<uint64_t> packetsReceived{0};      // All inbound packets
      std::atomic<uint64_t> packetsSent{0};          // All outbound packets  
      std::atomic<uint64_t> packetsDropped{0};       // Dropped due to queue overflow
      std::atomic<uint64_t> packetsCorrupted{0};     // Failed decode/validation

      // Connection metrics
      std::atomic<uint32_t> activeConnections{0};    // Current connection count
      std::atomic<uint64_t> totalConnections{0};     // Lifetime connections
      std::atomic<uint64_t> connectionsFailed{0};    // Failed connection attempts
      std::atomic<uint64_t> connectionsTimedOut{0};  // Keep-alive timeouts

      // Queue depths (sampled periodically)
      std::atomic<size_t> inboundQueueDepth{0};      // Packets waiting processing
      std::atomic<size_t> outboundQueueDepth{0};     // Packets waiting transmission
      std::atomic<size_t> maxInboundDepth{0};        // Peak inbound queue size
      std::atomic<size_t> maxOutboundDepth{0};       // Peak outbound queue size
};

Protocol State Tracking

    struct ProtocolMetrics {
    // State transition counts
    std::atomic<uint64_t> handshakeTransitions{0};   // HANDSHAKING → LOGIN/STATUS
    std::atomic<uint64_t> loginTransitions{0};       // LOGIN → PLAY  
    std::atomic<uint64_t> compressionFlips{0};       // SetCompression applied
    std::atomic<uint64_t> encryptionFlips{0};        // Encryption enabled

      // Pipeline flip timing
      std::atomic<float> avgCompressionFlipTime{0.0f}; // Microseconds
      std::atomic<float> avgStateTransitionTime{0.0f}; // Microseconds

      // Write in-flight tracking
      std::atomic<uint32_t> writesInFlight{0};         // Concurrent async_write operations
      std::atomic<uint32_t> maxWritesInFlight{0};      // Peak concurrent writes
      std::atomic<uint64_t> writeCompletions{0};       // Total async_write completions
      std::atomic<uint64_t> writeFailures{0};          // Failed async_write operations

      // Keep-alive RTT tracking
      std::atomic<float> averageKeepAliveRTT{0.0f};    // Moving average RTT in milliseconds
      std::atomic<float> p95KeepAliveRTT{0.0f};        // 95th percentile RTT
      std::atomic<uint64_t> keepAlivesSent{0};         // Total keep-alive packets sent
      std::atomic<uint64_t> keepAlivesReceived{0};     // Total keep-alive responses received

      // Stalled write detection
      std::atomic<float> lastWriteCompletionAge{0.0f}; // Time since last async_write completion (ms)
      std::atomic<uint32_t> stalledConnections{0};     // Connections with stalled writes
};

Mesh Building Metrics

    struct MeshMetrics {
    // Build counts by layer
    std::atomic<uint64_t> opaqueMeshBuilds{0};
    std::atomic<uint64_t> cutoutMeshBuilds{0};
    std::atomic<uint64_t> translucentMeshBuilds{0};
    std::atomic<uint64_t> emissiveMeshBuilds{0};

      // Timing metrics
      std::atomic<float> avgMeshBuildTime{0.0f};       // Milliseconds per section
      std::atomic<float> maxMeshBuildTime{0.0f};       // Worst case build time
      std::atomic<uint64_t> meshBuildsOverBudget{0};   // Builds exceeding time limit

      // Memory metrics  
      std::atomic<uint64_t> totalVerticesGenerated{0}; // Lifetime vertex count
      std::atomic<uint64_t> totalIndicesGenerated{0};  // Lifetime index count
      std::atomic<size_t> currentMeshMemoryUsage{0};   // Bytes in mesh buffers
      std::atomic<size_t> peakMeshMemoryUsage{0};      // Peak memory usage

      // Upload metrics
      std::atomic<uint64_t> gpuUploadsCompleted{0};    // VBO/IBO uploads
      std::atomic<uint64_t> gpuUploadFailures{0};      // Failed uploads
      std::atomic<float> avgUploadTime{0.0f};          // Milliseconds per upload
      std::atomic<uint64_t> uploadsOverBudget{0};      // Uploads exceeding time limit
    };

Chunk Streaming Metrics

    struct ChunkMetrics {
    // Chunk lifecycle counts
    std::atomic<uint64_t> chunksLoaded{0};           // Loaded from storage/generation
    std::atomic<uint64_t> chunksSaved{0};            // Saved to storage
    std::atomic<uint64_t> chunksGenerated{0};        // Procedurally generated
    std::atomic<uint64_t> chunksUnloaded{0};         // Unloaded from memory

      // Chunk sending
      std::atomic<uint64_t> chunksSent{0};             // ChunkDataS2C packets sent
      std::atomic<uint64_t> chunksUnloadSent{0};       // UnloadChunkS2C packets sent
      std::atomic<uint64_t> chunkSendFailures{0};      // Failed to send chunks

      // Performance metrics
      std::atomic<float> avgChunkLoadTime{0.0f};       // Milliseconds per chunk
      std::atomic<float> avgChunkSaveTime{0.0f};       // Milliseconds per save
      std::atomic<float> avgChunkGenerateTime{0.0f};   // Milliseconds per generation

      // Memory tracking
      std::atomic<size_t> chunksInMemory{0};           // Currently loaded chunks
      std::atomic<size_t> chunkMemoryUsage{0};         // Total chunk memory (bytes)
      std::atomic<size_t> peakChunksInMemory{0};       // Peak loaded chunks
    };

Logging Implementation

Periodic Performance Log

    class PerformanceLogger {
    private:
    std::chrono::steady_clock::time_point m_lastLogTime;
    const std::chrono::seconds m_logInterval{10}; // Log every 10 seconds
    
    public:
    void Update() {
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastLogTime >= m_logInterval) {
    LogPerformanceMetrics();
    m_lastLogTime = now;
    }
    }
    
    private:
    void LogPerformanceMetrics() {
    const auto& network = GetNetworkMetrics();
    const auto& protocol = GetProtocolMetrics();
    const auto& mesh = GetMeshMetrics();
    const auto& chunk = GetChunkMetrics();

          // Network performance
          Log::Info("Network: %.1f KB/s in, %.1f KB/s out, %d active connections",
                    CalculateKBPerSecond(network.bytesReceived.load()),
                    CalculateKBPerSecond(network.bytesSent.load()),
                    network.activeConnections.load());

          // Queue health
          Log::Info("Queues: inbound=%zu (peak=%zu), outbound=%zu (peak=%zu)",
                    network.inboundQueueDepth.load(), network.maxInboundDepth.load(),
                    network.outboundQueueDepth.load(), network.maxOutboundDepth.load());

          // Protocol state
          Log::Info("Protocol: %d writes in-flight (peak=%d), compression flips=%d",
                    protocol.writesInFlight.load(), protocol.maxWritesInFlight.load(),
                    protocol.compressionFlips.load());

          // Keep-alive and connection health
          Log::Info("Keep-alive: avg RTT=%.1fms, p95 RTT=%.1fms, stalled=%d",
                    protocol.averageKeepAliveRTT.load(), protocol.p95KeepAliveRTT.load(),
                    protocol.stalledConnections.load());

          // Mesh building
          Log::Info("Mesh: avg build=%.2fms, uploads=%d, memory=%.1f MB",
                    mesh.avgMeshBuildTime.load(),
                    mesh.gpuUploadsCompleted.load(),
                    mesh.currentMeshMemoryUsage.load() / (1024.0f * 1024.0f));

          // Chunk streaming
          Log::Info("Chunks: %d loaded, avg load=%.2fms, memory=%.1f MB",
                    chunk.chunksInMemory.load(),
                    chunk.avgChunkLoadTime.load(),
                    chunk.chunkMemoryUsage.load() / (1024.0f * 1024.0f));
      }
};

Critical Event Logging

    // Log important state changes immediately
    class EventLogger {
    public:
    static void LogConnectionEvent(const std::string& event, const ServerConnection& conn) {
    Log::Info("[CONNECTION] %s: player='%s' id=%d from=%s",
    event.c_str(), conn.GetPlayerName().c_str(),
    conn.GetPlayerId(), conn.GetRemoteAddress().c_str());
    }

      static void LogProtocolFlip(const std::string& flipType, uint32_t connectionId, 
                                 float durationMicros) {
          Log::Info("[PROTOCOL] %s flip: conn=%d, duration=%.1fμs",
                    flipType.c_str(), connectionId, durationMicros);
      }

      static void LogPerformanceViolation(const std::string& system, 
                                         float actualMs, float budgetMs) {
          float percentOver = ((actualMs - budgetMs) / budgetMs) * 100.0f;
          Log::Warning("[PERFORMANCE] %s over budget: %.2fms vs %.2fms (%.1f%% over)",
                      system.c_str(), actualMs, budgetMs, percentOver);
      }

      static void LogResourceExhaustion(const std::string& resource, 
                                       size_t current, size_t limit) {
          float percentUsed = (static_cast<float>(current) / limit) * 100.0f;
          Log::Warning("[RESOURCE] %s exhaustion: %zu/%zu (%.1f%% used)",
                      resource.c_str(), current, limit, percentUsed);
      }
};

Debug Trace Logging (Development Only)

    #ifdef DEBUG_TRACE_ENABLED
    class TraceLogger {
    public:
    static void LogPacketTrace(const std::string& direction, uint8_t packetId,
    size_t payloadSize, uint32_t connectionId) {
    Log::Trace("[PACKET] %s 0x%02X (%s) size=%zu conn=%d",
    direction.c_str(), packetId, PacketIdToString(static_cast<PacketId>(packetId)),
    payloadSize, connectionId);
    }

      static void LogMeshTrace(ChunkPos chunkPos, int sectionY, 
                              const std::string& stage, float timeMs) {
          Log::Trace("[MESH] chunk=(%d,%d) section=%d stage=%s time=%.2fms",
                     chunkPos.x, chunkPos.z, sectionY, stage.c_str(), timeMs);
      }

      static void LogChunkTrace(ChunkPos chunkPos, const std::string& operation, 
                               const std::string& result) {
          Log::Trace("[CHUNK] (%d,%d) %s: %s",
                     chunkPos.x, chunkPos.z, operation.c_str(), result.c_str());
      }
    };
    #else
    // No-op versions for release builds
    class TraceLogger {
    public:
    static void LogPacketTrace(...) {}
    static void LogMeshTrace(...) {}
    static void LogChunkTrace(...) {}
    };
    #endif

Metrics Collection Integration

Counter Update Points

    // In ServerConnection::OnPacketReceived()
    void ServerConnection::OnPacketReceived(uint8_t packetId, const std::vector<uint8_t>& payload) {
    // Update metrics
    auto& metrics = GetNetworkMetrics();
    metrics.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    metrics.bytesReceived.fetch_add(payload.size(), std::memory_order_relaxed);

      // Update queue depth
      metrics.inboundQueueDepth.store(m_inboundPackets.Size(), std::memory_order_relaxed);

      // Track packet processing
      auto startTime = std::chrono::high_resolution_clock::now();

      try {
          ProcessPacket(packetId, payload);

          // Log trace (debug builds only)
          TraceLogger::LogPacketTrace("RECV", packetId, payload.size(), GetConnectionId());

      } catch (const std::exception& e) {
          metrics.packetsCorrupted.fetch_add(1, std::memory_order_relaxed);
          Log::Warning("Packet processing failed: %s", e.what());
      }

      auto endTime = std::chrono::high_resolution_clock::now();
      float processingTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

      // Update processing time average
      UpdateMovingAverage(GetProtocolMetrics().avgPacketProcessingTime, processingTime);
    }

    // In ServerConnection::OnKeepAliveReceived()
    void ServerConnection::OnKeepAliveReceived(const KeepAliveC2SPacket& packet) {
        auto now = std::chrono::steady_clock::now();
        auto it = m_pendingKeepAlives.find(packet.keepAliveId);
        
        if (it != m_pendingKeepAlives.end()) {
            // Calculate RTT
            auto sentTime = it->second;
            float rttMs = std::chrono::duration<float, std::milli>(now - sentTime).count();
            
            // Update metrics
            auto& protocol = GetProtocolMetrics();
            protocol.keepAlivesReceived.fetch_add(1, std::memory_order_relaxed);
            UpdateMovingAverage(protocol.averageKeepAliveRTT, rttMs);
            UpdatePercentile(protocol.p95KeepAliveRTT, rttMs, 0.95f);
            
            m_pendingKeepAlives.erase(it);
            TraceLogger::LogPacketTrace("KEEPALIVE_RTT", 0, 0, GetConnectionId(), rttMs);
        }
    }

    // In ServerConnection::CheckStalledWrites()
    void ServerConnection::CheckStalledWrites() {
        auto now = std::chrono::steady_clock::now();
        float ageMs = std::chrono::duration<float, std::milli>(now - m_lastWriteCompletion).count();
        
        auto& protocol = GetProtocolMetrics();
        protocol.lastWriteCompletionAge.store(ageMs, std::memory_order_relaxed);
        
        // Detect stalled writes (no completion in >5 seconds with writes pending)
        if (ageMs > 5000.0f && protocol.writesInFlight.load() > 0) {
            if (!m_isStalled) {
                protocol.stalledConnections.fetch_add(1, std::memory_order_relaxed);
                m_isStalled = true;
                Log::Warning("Connection %d has stalled writes (%.1fs since last completion)",
                           GetConnectionId(), ageMs / 1000.0f);
            }
        } else if (m_isStalled && ageMs < 1000.0f) {
            // Connection recovered
            protocol.stalledConnections.fetch_sub(1, std::memory_order_relaxed);
            m_isStalled = false;
            Log::Info("Connection %d recovered from stalled writes", GetConnectionId());
        }
    }

    // In ClientMeshManager::UploadSectionMesh()
    void ClientMeshManager::UploadSectionMesh(ChunkPos chunkPos, int sectionY,
    const MeshBuildResult& result) {
    auto startTime = std::chrono::high_resolution_clock::now();

      try {
          // Perform GPU upload
          UploadMeshDataToGPU(result);

          // Update success metrics
          auto& metrics = GetMeshMetrics();
          metrics.gpuUploadsCompleted.fetch_add(1, std::memory_order_relaxed);
          metrics.totalVerticesGenerated.fetch_add(result.vertexCount, std::memory_order_relaxed);

          TraceLogger::LogMeshTrace(chunkPos, sectionY, "UPLOAD_SUCCESS", 0.0f);

      } catch (const std::exception& e) {
          GetMeshMetrics().gpuUploadFailures.fetch_add(1, std::memory_order_relaxed);
          Log::Error("Mesh upload failed: %s", e.what());
      }

      auto endTime = std::chrono::high_resolution_clock::now();
      float uploadTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

      UpdateMovingAverage(GetMeshMetrics().avgUploadTime, uploadTime);

      if (uploadTime > GetMeshBudgets().gpuUploadBudgetPerSection) {
          GetMeshMetrics().uploadsOverBudget.fetch_add(1, std::memory_order_relaxed);
          EventLogger::LogPerformanceViolation("mesh_upload", uploadTime,
                                             GetMeshBudgets().gpuUploadBudgetPerSection);
      }
}

Exponential Moving Average Helper

    template<typename T>
    void UpdateMovingAverage(std::atomic<T>& average, T newValue, float alpha = 0.1f) {
    T current = average.load(std::memory_order_relaxed);
    T updated = current * (1.0f - alpha) + newValue * alpha;
    average.store(updated, std::memory_order_relaxed);
    }

External Monitoring Integration

OpenTelemetry Integration (Optional)

    #ifdef ENABLE_OPENTELEMETRY
    class TelemetryExporter {
    private:
    std::unique_ptr<opentelemetry::metrics::MeterProvider> m_meterProvider;
    std::unique_ptr<opentelemetry::metrics::Meter> m_meter;

    public:
    void Initialize() {
    // Setup OpenTelemetry meter
    m_meterProvider = opentelemetry::metrics::MeterProviderFactory::Create();
    m_meter = m_meterProvider->GetMeter("myvoxelgame", "1.0.0");

          // Create metric instruments
          auto bytesCounter = m_meter->CreateUInt64Counter("network_bytes_total");
          auto latencyHistogram = m_meter->CreateDoubleHistogram("packet_latency_ms");
          auto frameTimeGauge = m_meter->CreateDoubleGauge("frame_time_ms");
      }

      void ExportMetrics() {
          const auto& network = GetNetworkMetrics();
          const auto& mesh = GetMeshMetrics();

          // Export counters
          m_bytesCounter->Add(network.bytesReceived.load(), {{"direction", "inbound"}});
          m_bytesCounter->Add(network.bytesSent.load(), {{"direction", "outbound"}});

          // Export gauges
          m_frameTimeGauge->Set(GetLastFrameTime(), {{"thread", "render"}});
          m_frameTimeGauge->Set(GetLastTickTime(), {{"thread", "server"}});

          // Export histograms
          m_latencyHistogram->Record(CalculateAverageLatency());
      }
    };
    #endif

Prometheus Metrics Endpoint

    #ifdef ENABLE_PROMETHEUS
    class PrometheusExporter {
    private:
    prometheus::Registry m_registry;
    std::unique_ptr<prometheus::Gateway> m_gateway;

    public:
    void Initialize() {
    // Create Prometheus gateway
    m_gateway = std::make_unique<prometheus::Gateway>("localhost:9091", "myvoxelgame");

          // Register metrics
          auto& networkFamily = prometheus::BuildCounter()
              .Name("myvoxelgame_network_bytes_total")
              .Help("Total network bytes transferred")
              .Register(m_registry);

          m_networkBytesIn = &networkFamily.Add({{"direction", "in"}});
          m_networkBytesOut = &networkFamily.Add({{"direction", "out"}});
      }

      void PushMetrics() {
          // Update Prometheus counters
          const auto& metrics = GetNetworkMetrics();
          m_networkBytesIn->Set(metrics.bytesReceived.load());
          m_networkBytesOut->Set(metrics.bytesSent.load());

          // Push to Prometheus pushgateway
          auto result = m_gateway->Push(m_registry);
          if (result != prometheus::Gateway::HttpStatusCode::Ok) {
              Log::Warning("Failed to push metrics to Prometheus: %d", static_cast<int>(result));
          }
      }
    };
    #endif

This comprehensive logging and metrics system provides deep visibility into engine performance while maintaining low overhead through atomic operations and careful sampling strategies.
