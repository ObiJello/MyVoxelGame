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
        bool player = true;
        bool renderControls = true;
        bool chunkViz = false;
        bool textureAtlas = false;
        bool logConsole = false;
        bool controls = false;
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

        // Helpers
        static bool IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos);
        static void ApplyRenderingMode(bool useMinecraftStyle);
        static const char* GetDirectionName(float yaw);

        // State
        static PanelVisibility s_visibility;
        static LogBuffer s_logBuffer;
        static ServerMetricsSnapshot s_serverSnap;
        static NetworkMetricsSnapshot s_netSnap;
        static bool s_debugEnabled;
    };

} // namespace Debug
