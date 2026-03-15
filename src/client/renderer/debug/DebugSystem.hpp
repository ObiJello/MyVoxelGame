// File: src/client/renderer/debug/DebugSystem.hpp
#pragma once

#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "../../input/PlayerController.hpp"
#include "../../entity/Player.hpp"
#include "../core/Vertex.hpp"
#include "common/core/Log.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <deque>

namespace Debug {

    // ========================================================================
    // PERFORMANCE METRICS (populated by PlatformMain, used in all builds)
    // ========================================================================

    struct PerformanceMetrics {
        // Overall frame timing
        float frameTime = 0.0f;

        // Detailed phase timings (all in milliseconds)
        float networkProcessingTime = 0.0f;
        float meshResultProcessingTime = 0.0f;
        float inputHandlingTime = 0.0f;
        float gameLogicTime = 0.0f;
        float meshSchedulingTime = 0.0f;
        float gpuUploadTime = 0.0f;
        float renderTime = 0.0f;
        float debugUITime = 0.0f;
        float vsyncWaitTime = 0.0f;
        float textureAnimationTime = 0.0f;
        float otherTime = 0.0f;

        // GPU timing (from ChunkRenderer GPU timer queries, 1-frame latency)
        float gpuOpaqueTimeMs = 0.0f;
        float gpuCutoutTimeMs = 0.0f;
        float gpuTranslucentTimeMs = 0.0f;
        float gpuTotalTimeMs = 0.0f;

        // Frame spike detection
        static constexpr int MAX_SPIKES = 8;
        struct FrameSpike {
            float totalMs = 0.0f;
            float renderMs = 0.0f;
            float meshSchedMs = 0.0f;
            float gpuUploadMs = 0.0f;
            float gpuTimeMs = 0.0f;
            int drawCalls = 0;
            int sectionsRendered = 0;
            float secondsAgo = 0.0f;  // Updated each frame
        };
        FrameSpike recentSpikes[MAX_SPIKES] = {};
        int spikeCount = 0;
        float targetFrameTimeMs = 16.67f; // 60fps target

        void RecordSpike(const FrameSpike& spike);
        float Get99thPercentile() const;
        float Get1PercentLow() const;
        int CountSpikesInHistory() const;  // frames above target in frameTimes[]

        int meshesUploadedThisFrame = 0;
        int meshesRenderedThisFrame = 0;
        size_t totalVerticesRendered = 0;
        size_t totalIndicesRendered = 0;

        int opaqueMeshesRendered = 0;
        int cutoutMeshesRendered = 0;
        int translucentMeshesRendered = 0;
        float meshBuildTimeMs = 0.0f;
        size_t totalMeshMemoryBytes = 0;

        // Rolling averages (values stored in MILLISECONDS)
        static constexpr int SAMPLE_COUNT = 300;
        float frameTimes[SAMPLE_COUNT] = {0};
        int sampleIndex = 0;

        void AddFrameTimeSample(float timeMs);
        float GetAverageFrameTime() const;
        float GetFPS() const;

        void ResetFrameMetrics() {
            meshesUploadedThisFrame = 0;
            meshesRenderedThisFrame = 0;
            totalVerticesRendered = 0;
            totalIndicesRendered = 0;
            opaqueMeshesRendered = 0;
            cutoutMeshesRendered = 0;
            translucentMeshesRendered = 0;
            meshBuildTimeMs = 0.0f;
        }
    };

    // ========================================================================
    // CROSS-THREAD METRIC SNAPSHOTS
    // ========================================================================

    struct ServerMetricsSnapshot {
        bool serverRunning = false;
        uint16_t serverPort = 0;
        int32_t worldSeed = 0;
        uint64_t ticksProcessed = 0;
        uint64_t chunksLoaded = 0;
        uint64_t chunksSent = 0;
        uint64_t blockChangesProcessed = 0;
        uint64_t packetsReceived = 0;
        uint64_t packetsSent = 0;
        float averageTickTime = 0.0f;
        float averageTPS = 20.0f;

        // Server worker pool
        size_t serverWorkerCount = 0;
        size_t serverPendingJobs = 0;
        size_t serverActiveJobs = 0;
        size_t serverChunksGenerated = 0;
        size_t serverChunksLoaded = 0;
        size_t serverChunksSaved = 0;
        size_t serverJobsSubmitted = 0;
        size_t serverJobsCompleted = 0;
        size_t serverJobsCancelled = 0;
        size_t serverJobsFailed = 0;

        // Chunk streaming
        size_t chunkProviderLoaded = 0;
        size_t chunkSenderPending = 0;
        size_t chunksPendingLoad = 0;
        float chunkSendRate = 9.0f;
        int chunkSenderUnacked = 0;

        // Player session
        size_t sessionWatchSetSize = 0;
        size_t sessionSentChunks = 0;
        int sessionViewDistance = 0;
    };

    struct NetworkMetricsSnapshot {
        bool connected = false;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        size_t incomingQueueSize = 0;
        size_t droppedPacketCount = 0;
        float connectionUptimeSec = 0.0f;
    };

    struct ChunkPipelineSnapshot {
        // View Distance
        int viewDistance = 0;
        int serverViewDistance = 0;
        size_t watchSetSize = 0;

        // Generation Queue (server workers)
        size_t sessionPendingLoads = 0;    // Chunks waiting for generation
        size_t serverPendingLoads = 0;     // Submitted to worker pool
        size_t workerThreads = 0;
        size_t workerPendingJobs = 0;
        size_t workerActiveJobs = 0;
        size_t chunksGenerated = 0;
        size_t chunksLoadedFromDisk = 0;
        size_t jobsFailed = 0;

        // Provider Cache
        size_t providerLoaded = 0;
        size_t providerMaxSize = 0;
        size_t providerEvictions = 0;

        // Send Queue (per-player chunk sender)
        size_t readyToSend = 0;            // m_pendingChunksToSend
        size_t sentToClient = 0;           // m_sentChunks
        float sendRate = 0.0f;             // desiredChunksPerTick
        float batchQuota = 0.0f;
        int unackedBatches = 0;
        int maxUnackedBatches = 0;

        // Client Receive
        uint64_t clientChunksReceived = 0;
        uint64_t clientChunksUnloaded = 0;
        float clientDesiredRate = 0.0f;    // What client asks server
        float clientAvgNanosPerChunk = 0.0f;

        // Client Mesh Pipeline
        size_t clientChunkCount = 0;
        size_t meshBuildsScheduled = 0;
        size_t meshBuildsCompleted = 0;
        size_t meshPendingJobs = 0;
        size_t meshActiveJobs = 0;
        size_t gpuActiveSections = 0;
        int meshUploadsThisFrame = 0;

        // Rendering
        int sectionsRendered = 0;
        int sectionsCulled = 0;
        int totalDrawCalls = 0;
        float renderTimeMs = 0.0f;
    };

    // ========================================================================
    // LOG CONSOLE BUFFER
    // ========================================================================

    struct LogEntry {
        Log::Level level;
        std::string message;
        float timestamp; // seconds since program start
    };

    class LogBuffer {
    public:
        static constexpr size_t MAX_ENTRIES = 2048;

        void Push(Log::Level level, const std::string& msg);

        // Returns a copy of entries for safe iteration (caller is on render thread)
        std::deque<LogEntry> GetEntries() const;
        void Clear();
        size_t Size() const;

    private:
        mutable std::mutex m_mutex;
        std::deque<LogEntry> m_entries;
        std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();
    };

    // ========================================================================
    // PANEL VISIBILITY
    // ========================================================================

    struct PanelVisibility {
        bool f3Overlay = false;
        bool performance = true;
        bool serverNetwork = false;
        bool clientSystems = false;
        bool memory = false;
        bool player = false;
        bool renderControls = false;
        bool chunkViz = false;
        bool textureAtlas = false;
        bool logConsole = false;
        bool controls = false;
        bool chunkPipeline = false;
    };

    // ========================================================================
    // DEBUG SYSTEM
    // ========================================================================

    class DebugSystem {
    public:
        static bool Initialize(GLFWwindow* window);
        static void Shutdown();

        static void RenderDebugUI(
            const Render::Camera& camera,
            const Frustum& frustum,
            Game::ClientPlayer& player,
            Game::ClientPlayerController& playerController,
            const PerformanceMetrics& metrics,
            bool cursorEnabled,
            int windowWidth, int windowHeight,
            int framebufferWidth, int framebufferHeight
        );

        static void BeginFrame();
        static void EndFrame();

        // Call from PlatformMain to populate cross-thread metrics before RenderDebugUI
        static void SetServerSnapshot(const ServerMetricsSnapshot& snap);
        static void SetNetworkSnapshot(const NetworkMetricsSnapshot& snap);
        static void SetChunkPipelineSnapshot(const ChunkPipelineSnapshot& snap);

    private:
        // Style
        static void ApplyCustomStyle();

        // F3 Overlay
        static void DrawF3Overlay(
            const Render::Camera& camera,
            const Game::ClientPlayer& player,
            const PerformanceMetrics& metrics,
            const ServerMetricsSnapshot& serverSnap
        );

        // Panels
        static void DrawMenuBar();
        static void DrawPerformancePanel(const PerformanceMetrics& metrics);
        static void DrawServerNetworkPanel(const ServerMetricsSnapshot& serverSnap, const NetworkMetricsSnapshot& netSnap);
        static void DrawClientSystemsPanel();
        static void DrawMemoryPanel(const PerformanceMetrics& metrics);
        static void DrawPlayerPanel(Game::ClientPlayer& player, Game::ClientPlayerController& playerController, const Render::Camera& camera);
        static void DrawRenderControlsPanel(int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight);
        static void DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum);
        static void DrawTextureAtlasDebug();
        static void DrawAtlasBuilderDebug();
        static void DrawWorldDebug();
        static void DrawLogConsolePanel();
        static void DrawControlsPanel(bool cursorEnabled, const Render::Camera& camera);
        static void DrawChunkPipelinePanel();

        // Helpers
        static bool IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos);
        static void ApplyRenderingMode(bool useMinecraftStyle);
        static const char* GetDirectionName(float yaw);

        // State
        static PanelVisibility s_visibility;
        static LogBuffer s_logBuffer;
        static ServerMetricsSnapshot s_serverSnap;
        static NetworkMetricsSnapshot s_netSnap;
        static ChunkPipelineSnapshot s_pipelineSnap;
        static bool s_debugEnabled;
        static bool s_renderDistanceChanged;

    public:
        // Returns true once if render distance was changed via debug UI, then resets
        static bool ConsumeRenderDistanceChanged();
    };

} // namespace Debug
