// File: src/render/debug/DebugSystem.hpp
#pragma once

#include "../gfx/Camera.hpp"
#include "../gfx/Frustum.hpp"
#include "../../game/PlayerController.hpp"
#include "../Vertex.hpp"

#ifndef NDEBUG
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

namespace Debug {

    struct PerformanceMetrics {
        float frameTime = 0.0f;
        float meshUploadTime = 0.0f;
        float renderTime = 0.0f;
        int meshesUploadedThisFrame = 0;
        int meshesRenderedThisFrame = 0;
        size_t totalVerticesRendered = 0;
        size_t totalIndicesRendered = 0;

        // **NEW**: Enhanced mesh system metrics
        int opaqueMeshesRendered = 0;
        int cutoutMeshesRendered = 0;
        int translucentMeshesRendered = 0;
        float meshBuildTimeMs = 0.0f;
        size_t totalMeshMemoryBytes = 0;

        // Rolling averages
        static constexpr int SAMPLE_COUNT = 60;
        float frameTimes[SAMPLE_COUNT] = {0};
        int sampleIndex = 0;

        void AddFrameTimeSample(float time);
        float GetAverageFrameTime() const;
        float GetFPS() const;

        // **NEW**: Reset per-frame metrics
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

    class DebugSystem {
    public:
        static bool Initialize(GLFWwindow* window);
        static void Shutdown();

        // Call once per frame - handles all debug UI
        static void RenderDebugUI(
            const Render::Camera& camera,
            const Frustum& frustum,
            const Game::PlayerController& playerController,
            const PerformanceMetrics& metrics,
            bool cursorEnabled,
            int windowWidth, int windowHeight,
            int framebufferWidth, int framebufferHeight
        );

        // Start/end frame for ImGui
        static void BeginFrame();
        static void EndFrame();

    private:
        static void DrawMainDebugWindow(
            const Render::Camera& camera,
            const Game::PlayerController& playerController,
            const PerformanceMetrics& metrics,
            bool cursorEnabled,
            int windowWidth, int windowHeight,
            int framebufferWidth, int framebufferHeight
        );

        static void DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum);
        static void DrawTextureAtlasDebug();
        static void DrawAtlasBuilderDebug();
        static void DrawWorldDebug();

        static bool IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos);
    };

} // namespace Debug