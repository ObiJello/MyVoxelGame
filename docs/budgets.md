Performance Budgets

**Note**: These are MyVoxelGame engine-specific targets, not Minecraft's official budgets.

Frame and Tick Timing Targets

Client Render Thread (60 FPS Target)

- Frame Budget: 16.67ms (60 FPS), fallback to 33.33ms (30 FPS)
- Input Processing: 0.5-1.0ms max
- Packet Drain: 2.0-3.0ms max
- Mesh Scheduling: 1.0-1.5ms max
- GPU Upload: 2.0-3.0ms max
- Culling: 1.0-2.0ms max
- Rendering: 8.0-12.0ms max
- Present/VSync: 1.0-2.0ms max
- Frame Overhead: 1.0-2.0ms max

Total Frame Budget Breakdown:
- Input:     1ms  ████
- Packets:   3ms  ████████████
- Mesh:      1ms  ████
- GPU Up:    3ms  ████████████
- Culling:   2ms  ████████
- Render:   12ms  ████████████████████████████████████████████████
- Present:   2ms  ████████
- Overhead:  2ms  ████████
----
Total:   16ms (remaining: 0.67ms buffer)

Server Thread (20 TPS Fixed)

- Tick Budget: 50ms hard limit, 30ms target
- Connection Drain: 5-10ms max (scales with player count)
- Packet Processing: 5-8ms max
- World Simulation: 15-20ms max
- Chunk Management: 3-5ms max
- Outbound Packets: 2-4ms max
- Statistics: 0.5-1.0ms max

Tick Budget Breakdown:
- Connections:  10ms  ████████████████████
- Processing:    8ms  ████████████████
- World Sim:    20ms  ████████████████████████████████████████
- Chunks:        5ms  ██████████
- Outbound:      4ms  ████████
- Stats:         1ms  ██
----
Total:       48ms (remaining: 2ms buffer)

Component-Specific Budgets

Packet Processing Limits

    struct ClientPacketBudgets {
        // Per-frame limits (client)
        size_t maxPacketsPerFrame = 100;           // Total packets processed
        size_t maxChunkPacketsPerFrame = 10;       // ChunkDataS2C packets
        size_t maxBlockChangesPerFrame = 50;       // BlockChangeS2C packets
        float clientPacketProcessingBudget = 3.0f; // Milliseconds
    };

    struct ServerPacketBudgets {
        // Per-tick limits (server)  
        size_t maxPacketsPerTick = 200;            // Total C2S packets
        size_t maxPlayerMovesPerTick = 20;         // PlayerMoveC2S packets
        size_t maxBlockActionsPerTick = 10;        // BlockActionC2S packets
        float serverPacketProcessingBudget = 8.0f; // Milliseconds
    };

Mesh Building Budgets

    struct MeshBudgets {
        // Scheduling limits (per frame)
        int maxMeshBuildsPerFrame = 5;             // Jobs submitted to workers
        float meshSchedulingBudget = 1.0f;         // Milliseconds for scheduling logic

        // Worker thread limits
        int maxConcurrentMeshBuilds = 4;           // Parallel mesh builds
        float maxMeshBuildTime = 50.0f;            // Milliseconds per section

        // GPU upload limits (per frame)
        int maxGPUUploadsPerFrame = 3;             // VBO/IBO uploads
        float gpuUploadBudget = 3.0f;              // Milliseconds for uploads
        size_t maxVerticesPerUpload = 65536;       // Vertex buffer size limit
    };

Chunk Loading Budgets

    struct ChunkBudgets {
        // Server worker thread limits
        int maxChunkLoadsPerTick = 5;              // Chunks loaded per server tick
        float chunkLoadBudget = 20.0f;             // Milliseconds for chunk operations

        // File I/O limits
        size_t maxConcurrentReads = 8;             // Parallel .mca file reads
        float diskReadTimeout = 100.0f;            // Milliseconds per file read

        // Memory limits
        size_t maxLoadedChunks = 2048;             // Total chunks in memory
        size_t chunkUnloadBatchSize = 16;          // Chunks freed per cleanup cycle
    };

Rendering Pass Budgets

    struct RenderBudgets {
        // Per-pass time limits
        float opaquePassBudget = 6.0f;             // Milliseconds
        float cutoutPassBudget = 2.0f;             // Milliseconds  
        float translucentPassBudget = 3.0f;        // Milliseconds
        float emissivePassBudget = 1.0f;           // Milliseconds (if implemented)

        // Draw call limits
        size_t maxDrawCallsPerFrame = 1000;        // Total draw calls
        size_t maxSectionsPerPass = 500;           // Sections rendered per pass

        // Vertex limits  
        size_t maxVerticesPerFrame = 2000000;      // 2M vertices total
        size_t maxIndicesPerFrame = 4000000;       // 4M indices total
    };

Budget Enforcement Strategies

Frame Time Monitoring

    class FrameBudgetMonitor {
        private:
            std::chrono::steady_clock::time_point m_frameStart;
            float m_frameTarget = 16.67f; // 60 FPS
    
        public:
            void BeginFrame() {
                m_frameStart = std::chrono::steady_clock::now();
            }

            bool HasBudget(float budgetMs) const {
                auto elapsed = GetElapsedMs();
                return elapsed + budgetMs < m_frameTarget;
            }

            float GetElapsedMs() const {
                auto now = std::chrono::steady_clock::now();
                return std::chrono::duration<float, std::milli>(now - m_frameStart).count();
            }

            void EndFrame() {
                float frameTime = GetElapsedMs();
                if (frameTime > m_frameTarget * 1.1f) { // 10% over budget
                    Log::Warning("Frame over budget: %.2fms (target: %.2fms)",
                    frameTime, m_frameTarget);
                }
            }
    };

Adaptive Quality Control

    class AdaptiveQualityManager {
        private:
            float m_recentFrameTimes[60]; // Last 60 frames
            int m_frameIndex = 0;
    
        public:
            void UpdateQuality() {
            float avgFrameTime = CalculateAverageFrameTime();

          if (avgFrameTime > 20.0f) { // Consistently over 50 FPS
              // Reduce quality settings
              ReduceMeshBuildFrequency();
              ReduceRenderDistance();
              DisableExpensiveEffects();
          } else if (avgFrameTime < 12.0f) { // Consistently above 80 FPS
              // Increase quality settings  
              IncreaseMeshBuildFrequency();
              IncreaseRenderDistance();
              EnableExpensiveEffects();
          }
      }

    private:
    void ReduceMeshBuildFrequency() {
    MeshBudgets& budgets = GetMeshBudgets();
    budgets.maxMeshBuildsPerFrame = std::max(2, budgets.maxMeshBuildsPerFrame - 1);
    budgets.maxGPUUploadsPerFrame = std::max(1, budgets.maxGPUUploadsPerFrame - 1);
    }
    };

Budget-Aware Processing

    // Example: GPU upload with time budget
    void ClientMeshManager::PerformGPUUploads() {
        auto frameStart = std::chrono::steady_clock::now();
        const float budgetMs = m_config.gpuUploadBudget;

        int uploadsCompleted = 0;

        while (HasPendingUploads() && uploadsCompleted < m_config.maxGPUUploadsPerFrame) {
            // Check remaining budget
            auto elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - frameStart).count();

            if (elapsed > budgetMs) {
                Log::Debug("GPU upload budget exceeded, deferring remaining uploads");
                break;
            }

            // Upload next mesh
            auto result = GetNextUploadCandidate();
            UploadSectionMesh(result.chunkPos, result.sectionY, result);
            uploadsCompleted++;
        }

        m_stats.gpuUploadsThisFrame = uploadsCompleted;
        m_stats.gpuUploadTimeMs = elapsed;
    }

Performance Scaling

Player Count Scaling

    // Budget adjustments based on player count
    void UpdateBudgetsForPlayerCount(int playerCount) {
        ServerPacketBudgets& serverPackets = GetServerPacketBudgets();

        // Scale packet processing budget linearly with players
        serverPackets.maxPacketsPerTick = 200 + (playerCount - 1) * 50;
        serverPackets.serverPacketProcessingBudget = 8.0f + (playerCount - 1) * 2.0f;

        // Cap at reasonable limits
        serverPackets.maxPacketsPerTick = std::min(serverPackets.maxPacketsPerTick, 1000);
        serverPackets.serverPacketProcessingBudget = std::min(serverPackets.serverPacketProcessingBudget, 25.0f);
    }

View Distance Scaling

    // Adjust budgets based on render distance
    void UpdateBudgetsForViewDistance(int viewDistance) {
        MeshBudgets& mesh = GetMeshBudgets();
        RenderBudgets& render = GetRenderBudgets();
    
        // More chunks visible = more mesh work needed
        float scale = viewDistance / 8.0f; // 8 chunks = baseline
    
        mesh.maxMeshBuildsPerFrame = static_cast<int>(5 * scale);
        mesh.maxGPUUploadsPerFrame = static_cast<int>(3 * scale);
    
        render.maxDrawCallsPerFrame = static_cast<size_t>(1000 * scale * scale);
        render.maxVerticesPerFrame = static_cast<size_t>(2000000 * scale);
    }

Budget Violation Handling

Graceful Degradation

    class PerformanceDegrader {
    public:
    void HandleBudgetViolation(const std::string& system, float actualMs, float budgetMs) {
    float overage = actualMs - budgetMs;
    float percentOver = overage / budgetMs * 100.0f;

          Log::Warning("%s over budget by %.1f%% (%.2fms vs %.2fms)",
                      system.c_str(), percentOver, actualMs, budgetMs);

          if (percentOver > 50.0f) {
              // Severe overage - immediate action
              ApplyEmergencyDegradation(system);
          } else if (percentOver > 20.0f) {
              // Moderate overage - gradual reduction
              ApplyGradualDegradation(system);
          }
      }

    private:
    void ApplyEmergencyDegradation(const std::string& system) {
        if (system == "mesh_upload") {
            // Drastically reduce mesh work
            GetMeshBudgets().maxGPUUploadsPerFrame = 1;
            GetMeshBudgets().maxMeshBuildsPerFrame = 2;
        } else if (system == "rendering") {
            // Reduce render quality
            GetRenderBudgets().maxDrawCallsPerFrame /= 2;
            GetRenderBudgets().maxSectionsPerPass /= 2;
        }
    }
    };

Recovery Strategies

    class PerformanceRecovery {
    private:
    int m_stableFrames = 0;
    const int RECOVERY_THRESHOLD = 120; // 2 seconds at 60 FPS
    
    public:
    void OnFrameComplete(float frameTime, float target) {
        if (frameTime < target * 0.8f) { // Well under budget
            m_stableFrames++;
        } else {
            m_stableFrames = 0;
        }

        if (m_stableFrames >= RECOVERY_THRESHOLD) {
            AttemptQualityRecovery();
            m_stableFrames = 0;
        }
    }

    private:
    void AttemptQualityRecovery() {
        // Gradually restore quality settings
        MeshBudgets& mesh = GetMeshBudgets();
        if (mesh.maxMeshBuildsPerFrame < 5) {
            mesh.maxMeshBuildsPerFrame++;
        }
        if (mesh.maxGPUUploadsPerFrame < 3) {
            mesh.maxGPUUploadsPerFrame++;
        }
    }
    };

These budgets ensure the engine maintains smooth performance across different hardware configurations and usage scenarios, with automatic adaptation when performance targets are not met.
