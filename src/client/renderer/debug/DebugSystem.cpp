// File: src/client/renderer/debug/DebugSystem.cpp
#include "DebugSystem.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "../debug/Crosshair.hpp"
#include "../texture/AtlasBuilder.hpp"
#include "common/core/Config.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldGlobals.hpp"
#include "../../world/ClientChunkManager.hpp"
#include "../mesh/ClientMeshManager.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "../mesh/GPUDataPool.hpp"
#include "../backend/RenderBackend.hpp"
#include "../../world/ClientWorkerPool.hpp"
#include "../../input/Input.hpp"
#include <unordered_set>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Debug {

    // ========================================================================
    // PERFORMANCE METRICS
    // ========================================================================

    void PerformanceMetrics::AddFrameTimeSample(float timeMs) {
        frameTimes[sampleIndex] = timeMs;
        sampleIndex = (sampleIndex + 1) % SAMPLE_COUNT;
    }

    float PerformanceMetrics::GetAverageFrameTime() const {
        float sum = 0.0f;
        int count = 0;
        for (int i = 0; i < SAMPLE_COUNT; ++i) {
            if (frameTimes[i] > 0.0f) {
                sum += frameTimes[i];
                count++;
            }
        }
        return count > 0 ? sum / count : 0.0f;
    }

    float PerformanceMetrics::GetFPS() const {
        float avgTimeMs = GetAverageFrameTime();
        return avgTimeMs > 0.0f ? 1000.0f / avgTimeMs : 0.0f;
    }

    // ========================================================================
    // STATIC STATE
    // ========================================================================

    PanelVisibility DebugSystem::s_visibility;
    LogBuffer DebugSystem::s_logBuffer;
    ServerMetricsSnapshot DebugSystem::s_serverSnap;
    NetworkMetricsSnapshot DebugSystem::s_netSnap;
#ifdef NDEBUG
    bool DebugSystem::s_debugEnabled = false; // Release: hidden until Shift+Tab+D
#else
    bool DebugSystem::s_debugEnabled = true;  // Debug: always visible
#endif

    // ========================================================================
    // LOG BUFFER
    // ========================================================================

    void LogBuffer::Push(Log::Level level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_startTime).count();
        m_entries.push_back({level, msg, timeSec});
        while (m_entries.size() > MAX_ENTRIES) {
            m_entries.pop_front();
        }
    }

    std::deque<LogEntry> LogBuffer::GetEntries() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries;
    }

    void LogBuffer::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    size_t LogBuffer::Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    // Wrapper key callback that filters Tab (game control) before ImGui sees it
    static GLFWkeyfun s_prevKeyCallback = nullptr;
    static void FilteredKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (key == GLFW_KEY_TAB) return;
        if (s_prevKeyCallback) s_prevKeyCallback(window, key, scancode, action, mods);
    }

    bool DebugSystem::Initialize(GLFWwindow* window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        ImGui::StyleColorsDark();

        ApplyCustomStyle();

        // Route ImGui backend initialization through the render backend
        if (Render::g_renderBackend) {
            Render::g_renderBackend->ImGuiInit(window);
        } else {
            ImGui_ImplGlfw_InitForOpenGL(window, true);
            ImGui_ImplOpenGL3_Init("#version 330 core");
        }

        // Install wrapper callback that blocks Tab from reaching ImGui
        s_prevKeyCallback = glfwSetKeyCallback(window, FilteredKeyCallback);

        // Register log callback
        Log::RegisterCallback([](Log::Level level, const std::string& msg) {
            s_logBuffer.Push(level, msg);
        });

        Log::Info("Debug system initialized (professional rehaul)");
        return true;
    }

    void DebugSystem::Shutdown() {
        Log::UnregisterCallback();
        if (Render::g_renderBackend) {
            Render::g_renderBackend->ImGuiShutdown();
        } else {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
        }
        ImGui::DestroyContext();
        Log::Info("Debug system shutdown");
    }

    void DebugSystem::BeginFrame() {
        // Check toggle even when disabled so user can re-enable
        bool justToggled = false;
        if (Input::IsKeyDown(Input::Key::LeftShift) &&
            Input::IsKeyDown(Input::Key::Tilde) &&
            Input::IsKeyPressed(Input::Key::D)) {
            s_debugEnabled = !s_debugEnabled;
            justToggled = true;
            Log::Info(s_debugEnabled ? "Debug UI enabled" : "Debug UI disabled");
        }

        // Always run ImGui frame cycle to keep internal state consistent
        if (Render::g_renderBackend) {
            Render::g_renderBackend->ImGuiNewFrame();
        } else {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
        }
        ImGui::NewFrame();

        if (justToggled) {
            ImGui::SetWindowFocus(nullptr);
        }
    }

    void DebugSystem::EndFrame() {
        // Always end ImGui frame to match BeginFrame
        ImGui::Render();
        if (s_debugEnabled) {
            if (Render::g_renderBackend) {
                Render::g_renderBackend->ImGuiRender();
            } else {
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            }
        }
    }

    // ========================================================================
    // CUSTOM STYLE
    // ========================================================================

    void DebugSystem::ApplyCustomStyle() {
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding = 4.0f;
        style.FrameRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.ScrollbarRounding = 2.0f;
        style.TabRounding = 2.0f;

        style.WindowPadding = ImVec2(8, 8);
        style.FramePadding = ImVec2(4, 3);
        style.ItemSpacing = ImVec2(6, 4);
        style.ScrollbarSize = 12.0f;
        style.IndentSpacing = 16.0f;

        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg]           = ImVec4(0.10f, 0.10f, 0.13f, 0.95f);
        c[ImGuiCol_TitleBg]            = ImVec4(0.08f, 0.08f, 0.11f, 1.00f);
        c[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.15f, 0.22f, 1.00f);
        c[ImGuiCol_FrameBg]            = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
        c[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.30f, 1.00f);
        c[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.38f, 1.00f);
        c[ImGuiCol_Header]             = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
        c[ImGuiCol_HeaderHovered]      = ImVec4(0.28f, 0.28f, 0.38f, 1.00f);
        c[ImGuiCol_HeaderActive]       = ImVec4(0.32f, 0.32f, 0.45f, 1.00f);
        c[ImGuiCol_Button]             = ImVec4(0.20f, 0.35f, 0.55f, 1.00f);
        c[ImGuiCol_ButtonHovered]      = ImVec4(0.25f, 0.45f, 0.65f, 1.00f);
        c[ImGuiCol_ButtonActive]       = ImVec4(0.30f, 0.55f, 0.75f, 1.00f);
        c[ImGuiCol_Tab]                = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
        c[ImGuiCol_TabSelected]        = ImVec4(0.20f, 0.35f, 0.55f, 1.00f);
        c[ImGuiCol_TabHovered]         = ImVec4(0.28f, 0.45f, 0.65f, 1.00f);
        c[ImGuiCol_PlotLines]          = ImVec4(0.40f, 0.80f, 0.40f, 1.00f);
        c[ImGuiCol_PlotHistogram]      = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
        c[ImGuiCol_Separator]          = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
        c[ImGuiCol_CheckMark]          = ImVec4(0.40f, 0.80f, 0.40f, 1.00f);
        c[ImGuiCol_SliderGrab]         = ImVec4(0.30f, 0.55f, 0.80f, 1.00f);
        c[ImGuiCol_SliderGrabActive]   = ImVec4(0.40f, 0.65f, 0.90f, 1.00f);
    }

    // ========================================================================
    // CROSS-THREAD SNAPSHOT SETTERS (called from PlatformMain.cpp)
    // ========================================================================

    void DebugSystem::SetServerSnapshot(const ServerMetricsSnapshot& snap) {
        s_serverSnap = snap;
    }

    void DebugSystem::SetNetworkSnapshot(const NetworkMetricsSnapshot& snap) {
        s_netSnap = snap;
    }

    // ========================================================================
    // HELPERS
    // ========================================================================

    static const ImVec4 COL_GREEN  = ImVec4(0.30f, 0.90f, 0.30f, 1.0f);
    static const ImVec4 COL_YELLOW = ImVec4(1.00f, 0.90f, 0.30f, 1.0f);
    static const ImVec4 COL_RED    = ImVec4(1.00f, 0.30f, 0.30f, 1.0f);
    static const ImVec4 COL_BLUE   = ImVec4(0.40f, 0.70f, 1.00f, 1.0f);
    static const ImVec4 COL_ORANGE = ImVec4(1.00f, 0.70f, 0.30f, 1.0f);
    static const ImVec4 COL_GRAY   = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    static const ImVec4 COL_WHITE  = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);

    const char* DebugSystem::GetDirectionName(float yaw) {
        // Normalize yaw to 0-360
        float y = fmodf(yaw, 360.0f);
        if (y < 0) y += 360.0f;
        if (y >= 315 || y < 45) return "South (+Z)";
        if (y >= 45 && y < 135) return "West (-X)";
        if (y >= 135 && y < 225) return "North (-Z)";
        return "East (+X)";
    }

    static void FormatBytes(uint64_t bytes, char* buf, size_t bufSize) {
        if (bytes < 1024) {
            snprintf(buf, bufSize, "%llu B", (unsigned long long)bytes);
        } else if (bytes < 1024 * 1024) {
            snprintf(buf, bufSize, "%.1f KB", bytes / 1024.0);
        } else if (bytes < 1024ULL * 1024 * 1024) {
            snprintf(buf, bufSize, "%.2f MB", bytes / (1024.0 * 1024.0));
        } else {
            snprintf(buf, bufSize, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        }
    }

    // ========================================================================
    // MAIN RENDER ENTRY POINT
    // ========================================================================

    void DebugSystem::RenderDebugUI(
        const Render::Camera& camera,
        const Frustum& frustum,
        Game::ClientPlayer& player,
        Game::ClientPlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {

        // Disable ImGui mouse interaction when cursor is hidden (game mode)
        ImGuiIO& io = ImGui::GetIO();
        if (!cursorEnabled)
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

        if (!s_debugEnabled) return;

        // Handle F3 toggle
        if (Input::IsKeyPressed(Input::Key::F3)) {
            s_visibility.f3Overlay = !s_visibility.f3Overlay;
        }
        // Handle tilde for log console
        if (Input::IsKeyPressed(Input::Key::Tilde)) {
            s_visibility.logConsole = !s_visibility.logConsole;
        }

        // F3 overlay (rendered on foreground, always on top of game)
        if (s_visibility.f3Overlay) {
            DrawF3Overlay(camera, player, metrics, s_serverSnap);
        }

        // ImGui panels
        DrawMenuBar();

        if (s_visibility.performance)    DrawPerformancePanel(metrics);
        if (s_visibility.serverNetwork)  DrawServerNetworkPanel(s_serverSnap, s_netSnap);
        if (s_visibility.clientSystems)  DrawClientSystemsPanel();
        if (s_visibility.memory)         DrawMemoryPanel(metrics);
        if (s_visibility.player)         DrawPlayerPanel(player, playerController, camera);
        if (s_visibility.renderControls) DrawRenderControlsPanel(windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        if (s_visibility.chunkViz)       DrawChunkVisualization(camera, frustum);
        if (s_visibility.textureAtlas)   DrawTextureAtlasDebug();
        if (s_visibility.logConsole)     DrawLogConsolePanel();
        if (s_visibility.controls)       DrawControlsPanel(cursorEnabled, camera);
    }

    // ========================================================================
    // MENU BAR (floating window with panel toggles)
    // ========================================================================

    void DebugSystem::DrawMenuBar() {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(0, 0)); // Auto-size
        ImGui::Begin("Debug Panels", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::TextColored(COL_BLUE, "View Panels:");
        ImGui::Checkbox("Performance",     &s_visibility.performance);
        ImGui::SameLine();
        ImGui::Checkbox("Server/Network",  &s_visibility.serverNetwork);
        ImGui::SameLine();
        ImGui::Checkbox("Client Systems",  &s_visibility.clientSystems);

        ImGui::Checkbox("Memory",          &s_visibility.memory);
        ImGui::SameLine();
        ImGui::Checkbox("Player",          &s_visibility.player);
        ImGui::SameLine();
        ImGui::Checkbox("Render Controls", &s_visibility.renderControls);

        ImGui::Checkbox("Chunk Map",       &s_visibility.chunkViz);
        ImGui::SameLine();
        ImGui::Checkbox("Texture Atlas",   &s_visibility.textureAtlas);
        ImGui::SameLine();
        ImGui::Checkbox("Log Console (~)", &s_visibility.logConsole);
        ImGui::SameLine();
        ImGui::Checkbox("Controls",        &s_visibility.controls);

        ImGui::Separator();
        ImGui::TextColored(COL_GRAY, "F3: Overlay | ~: Log | Shift+~+D: Toggle UI");

        ImGui::End();
    }

    // ========================================================================
    // F3 OVERLAY
    // ========================================================================

    void DebugSystem::DrawF3Overlay(
        const Render::Camera& camera,
        const Game::ClientPlayer& player,
        const PerformanceMetrics& metrics,
        const ServerMetricsSnapshot& serverSnap) {

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float fontSize = ImGui::GetFontSize();
        float lineH = fontSize + 2.0f;
        ImFont* font = ImGui::GetFont();

        auto DrawLine = [&](float x, float y, const char* text, ImVec4 col = COL_WHITE) {
            ImU32 shadowCol = IM_COL32(0, 0, 0, 180);
            ImU32 textCol = ImGui::ColorConvertFloat4ToU32(col);
            dl->AddText(font, fontSize, ImVec2(x + 1, y + 1), shadowCol, text);
            dl->AddText(font, fontSize, ImVec2(x, y), textCol, text);
        };

        char buf[256];

        // --- Left column ---
        float lx = 10.0f, ly = 10.0f;

        snprintf(buf, sizeof(buf), "MyVoxelGame (Debug)");
        DrawLine(lx, ly, buf, COL_BLUE); ly += lineH;

        float fps = metrics.GetFPS();
        ImVec4 fpsCol = fps >= 55 ? COL_GREEN : (fps >= 30 ? COL_YELLOW : COL_RED);
        snprintf(buf, sizeof(buf), "%.0f FPS (%.1f ms)", fps, metrics.GetAverageFrameTime());
        DrawLine(lx, ly, buf, fpsCol); ly += lineH;

        ly += 4; // gap

        const auto& pos = player.physics.position;
        snprintf(buf, sizeof(buf), "XYZ: %.3f / %.3f / %.3f", pos.x, pos.y, pos.z);
        DrawLine(lx, ly, buf); ly += lineH;

        snprintf(buf, sizeof(buf), "Block: %d %d %d",
            (int)std::floor(pos.x), (int)std::floor(pos.y), (int)std::floor(pos.z));
        DrawLine(lx, ly, buf); ly += lineH;

        int cx = (int)std::floor(pos.x / 16.0f);
        int cz = (int)std::floor(pos.z / 16.0f);
        int secY = (int)std::floor((pos.y + 64.0f) / 16.0f);
        snprintf(buf, sizeof(buf), "Chunk: %d %d  Section: %d", cx, cz, secY);
        DrawLine(lx, ly, buf); ly += lineH;

        snprintf(buf, sizeof(buf), "Facing: %s (%.1f / %.1f)",
            GetDirectionName(camera.yaw), camera.yaw, camera.pitch);
        DrawLine(lx, ly, buf); ly += lineH;

        float speed = glm::length(player.physics.velocity);
        snprintf(buf, sizeof(buf), "Speed: %.2f blocks/s", speed);
        DrawLine(lx, ly, buf); ly += lineH;

        // --- Right column ---
        ImGuiIO& io = ImGui::GetIO();
        float rx = io.DisplaySize.x - 300.0f;
        float ry = 10.0f;

        size_t loadedChunks = Game::g_world ? Game::g_world->GetLoadedChunkCount() : 0;
        snprintf(buf, sizeof(buf), "Chunks: %zu loaded", loadedChunks);
        DrawLine(rx, ry, buf); ry += lineH;

        snprintf(buf, sizeof(buf), "Rendered: %d sections", metrics.meshesRenderedThisFrame);
        DrawLine(rx, ry, buf); ry += lineH;

        if (auto* rs = Render::GetChunkRendererStats()) {
            snprintf(buf, sizeof(buf), "Draw Calls: %d", rs->totalDrawCalls);
            DrawLine(rx, ry, buf); ry += lineH;

            snprintf(buf, sizeof(buf), "Culled: %d / %d checked",
                rs->sectionsSkipped, rs->sectionsAvailable);
            DrawLine(rx, ry, buf); ry += lineH;
        }

        ry += 4;

        snprintf(buf, sizeof(buf), "Server TPS: %.1f", serverSnap.averageTPS);
        ImVec4 tpsCol = serverSnap.averageTPS >= 19 ? COL_GREEN : (serverSnap.averageTPS >= 15 ? COL_YELLOW : COL_RED);
        DrawLine(rx, ry, buf, tpsCol); ry += lineH;

        snprintf(buf, sizeof(buf), "Tick: %.1f ms", serverSnap.averageTickTime);
        DrawLine(rx, ry, buf); ry += lineH;
    }

    // ========================================================================
    // PERFORMANCE PANEL
    // ========================================================================

    void DebugSystem::DrawPerformancePanel(const PerformanceMetrics& metrics) {
        ImGui::SetNextWindowPos(ImVec2(10, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Performance", &s_visibility.performance)) {
            ImGui::End();
            return;
        }

        // FPS header
        float fps = metrics.GetFPS();
        float avgMs = metrics.GetAverageFrameTime();
        ImVec4 fpsCol = fps >= 55 ? COL_GREEN : (fps >= 30 ? COL_YELLOW : COL_RED);
        ImGui::TextColored(fpsCol, "%.0f FPS", fps);
        ImGui::SameLine();
        ImGui::Text("(%.2f ms avg, %.2f ms frame)", avgMs, metrics.frameTime);

        ImGui::Separator();

        // Percentage helper
        auto pct = [&](float t) { return metrics.frameTime > 0 ? (t / metrics.frameTime * 100.0f) : 0.0f; };

        // Frame time breakdown bar
        float barWidth = ImGui::GetContentRegionAvail().x;
        float barHeight = 20.0f;
        ImVec2 barPos = ImGui::GetCursorScreenPos();

        struct BarSeg { float time; ImU32 color; const char* label; };
        BarSeg segs[] = {
            {metrics.networkProcessingTime,   IM_COL32(100, 160, 255, 255), "Net"},
            {metrics.inputHandlingTime,       IM_COL32(100, 200, 100, 255), "Input"},
            {metrics.gameLogicTime,           IM_COL32(100, 220, 180, 255), "Logic"},
            {metrics.meshResultProcessingTime,IM_COL32(180, 140, 255, 255), "MeshRes"},
            {metrics.meshSchedulingTime,      IM_COL32(140, 180, 255, 255), "MeshSch"},
            {metrics.gpuUploadTime,           IM_COL32(255, 180, 80,  255), "Upload"},
            {metrics.textureAnimationTime,    IM_COL32(200, 200, 100, 255), "TexAnim"},
            {metrics.renderTime,              IM_COL32(255, 140, 80,  255), "Render"},
            {metrics.debugUITime,             IM_COL32(150, 255, 150, 255), "DbgUI"},
            {metrics.vsyncWaitTime,           IM_COL32(100, 100, 100, 255), "Swap"},
        };

        if (metrics.frameTime > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float x = barPos.x;
            for (auto& s : segs) {
                float w = (s.time / metrics.frameTime) * barWidth;
                if (w > 0.5f) {
                    dl->AddRectFilled(ImVec2(x, barPos.y), ImVec2(x + w, barPos.y + barHeight), s.color);
                    if (w > 30.0f) {
                        dl->AddText(ImVec2(x + 2, barPos.y + 3), IM_COL32(0, 0, 0, 220), s.label);
                    }
                }
                x += w;
            }
            dl->AddRect(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight), IM_COL32(80, 80, 80, 255));
        }
        ImGui::Dummy(ImVec2(barWidth, barHeight + 4));

        // CPU Operations
        if (ImGui::CollapsingHeader("CPU Operations", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            ImGui::TextColored(COL_BLUE, "Network:       %6.2f ms (%4.1f%%)", metrics.networkProcessingTime, pct(metrics.networkProcessingTime));
            ImGui::TextColored(COL_BLUE, "Input:         %6.2f ms (%4.1f%%)", metrics.inputHandlingTime, pct(metrics.inputHandlingTime));
            ImGui::TextColored(COL_BLUE, "Game Logic:    %6.2f ms (%4.1f%%)", metrics.gameLogicTime, pct(metrics.gameLogicTime));
            ImGui::TextColored(COL_BLUE, "Mesh Results:  %6.2f ms (%4.1f%%)", metrics.meshResultProcessingTime, pct(metrics.meshResultProcessingTime));
            ImGui::TextColored(COL_BLUE, "Mesh Schedule: %6.2f ms (%4.1f%%)", metrics.meshSchedulingTime, pct(metrics.meshSchedulingTime));
            ImGui::TextColored(COL_BLUE, "Tex Animation: %6.2f ms (%4.1f%%)", metrics.textureAnimationTime, pct(metrics.textureAnimationTime));
            ImGui::Unindent();
        }

        // GPU Operations
        if (ImGui::CollapsingHeader("GPU Operations", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            ImGui::TextColored(COL_ORANGE, "GPU Upload:  %6.2f ms (%4.1f%%)", metrics.gpuUploadTime, pct(metrics.gpuUploadTime));
            ImGui::TextColored(COL_ORANGE, "Rendering:   %6.2f ms (%4.1f%%)", metrics.renderTime, pct(metrics.renderTime));

            if (ImGui::TreeNode("Render Breakdown")) {
                if (auto* rs = Render::GetChunkRendererStats()) {
                    ImGui::Text("Build Draw Lists:  %.2f ms", rs->buildDrawListsTimeMs);
                    if (ImGui::TreeNode("Build Details")) {
                        ImGui::Text("  Chunk Iteration: %.2f ms", rs->chunkIterationTimeMs);
                        ImGui::Text("  GPU Data Load:   %.2f ms", rs->gpuDataLoadTimeMs);
                        ImGui::Text("  Frustum Culling: %.2f ms", rs->frustumCullingTimeMs);
                        ImGui::Text("  Sorting:         %.2f ms", rs->sortingTimeMs);
                        ImGui::Text("  Sections: %d checked, %d culled, %d rendered",
                            rs->sectionsAvailable, rs->sectionsSkipped, rs->sectionsRendered);
                        ImGui::TreePop();
                    }
                    ImGui::Spacing();
                    ImGui::Text("Opaque Pass:       %.2f ms (%d sections)", rs->opaquePassTimeMs, rs->opaqueSections);
                    ImGui::Text("Cutout Pass:       %.2f ms (%d sections)", rs->cutoutPassTimeMs, rs->cutoutSections);
                    ImGui::Text("Translucent Pass:  %.2f ms (%d sections)", rs->translucentPassTimeMs, rs->translucentSections);
                    ImGui::Text("Draw Calls:        %d", rs->totalDrawCalls);

                    if (rs->gpuDataLoadTimeMs > 1.0f)
                        ImGui::TextColored(COL_RED, "! Atomic loads slow (%.2f ms)", rs->gpuDataLoadTimeMs);
                } else {
                    ImGui::TextDisabled("Render stats unavailable");
                }
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }

        // System
        if (ImGui::CollapsingHeader("System")) {
            ImGui::Indent();
            ImGui::Text("Debug UI:      %6.2f ms (%4.1f%%)", metrics.debugUITime, pct(metrics.debugUITime));
            ImGui::Text("Buffer Swap:   %6.2f ms (%4.1f%%)", metrics.vsyncWaitTime, pct(metrics.vsyncWaitTime));
            ImGui::Text("Unaccounted:   %6.2f ms (%4.1f%%)", metrics.otherTime, pct(metrics.otherTime));
            ImGui::Unindent();
        }

        // Bottleneck
        struct TimerEntry { const char* name; float time; };
        TimerEntry timers[] = {
            {"Network", metrics.networkProcessingTime}, {"Input", metrics.inputHandlingTime},
            {"Game Logic", metrics.gameLogicTime}, {"Mesh Results", metrics.meshResultProcessingTime},
            {"Mesh Schedule", metrics.meshSchedulingTime}, {"GPU Upload", metrics.gpuUploadTime},
            {"Rendering", metrics.renderTime}, {"Debug UI", metrics.debugUITime},
            {"Buffer Swap", metrics.vsyncWaitTime}
        };
        auto* bottleneck = &timers[0];
        for (auto& t : timers) { if (t.time > bottleneck->time) bottleneck = &t; }

        if (bottleneck->time > 10.0f) {
            ImGui::TextColored(COL_RED, "BOTTLENECK: %s (%.1f ms)", bottleneck->name, bottleneck->time);
        } else if (bottleneck->time > 5.0f) {
            ImGui::TextColored(COL_YELLOW, "Slowest: %s (%.1f ms)", bottleneck->name, bottleneck->time);
        }

        // Frame time graph
        ImGui::Separator();
        float maxTime = 0.0f;
        for (int i = 0; i < PerformanceMetrics::SAMPLE_COUNT; i++) {
            if (metrics.frameTimes[i] > maxTime) maxTime = metrics.frameTimes[i];
        }
        maxTime = std::max(maxTime * 1.2f, 16.67f); // At least 60fps scale
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.1f ms avg", avgMs);
        ImGui::PlotLines("##FrameTime", metrics.frameTimes, PerformanceMetrics::SAMPLE_COUNT,
            metrics.sampleIndex, overlay, 0.0f, maxTime, ImVec2(0, 60));

        // Geometry stats
        ImGui::Text("~Vertices: %zu  Indices: %zu", metrics.totalVerticesRendered, metrics.totalIndicesRendered);
        ImGui::Text("By Layer: Opaque %d | Cutout %d | Translucent %d",
            metrics.opaqueMeshesRendered, metrics.cutoutMeshesRendered, metrics.translucentMeshesRendered);

        ImGui::End();
    }

    // ========================================================================
    // SERVER & NETWORK PANEL
    // ========================================================================

    void DebugSystem::DrawServerNetworkPanel(const ServerMetricsSnapshot& srv, const NetworkMetricsSnapshot& net) {
        ImGui::SetNextWindowPos(ImVec2(440, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 450), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Server & Network", &s_visibility.serverNetwork)) {
            ImGui::End();
            return;
        }

        // Server status
        if (ImGui::CollapsingHeader("Server", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (srv.serverRunning) {
                ImGui::TextColored(COL_GREEN, "Server: Running (port %u)", srv.serverPort);
            } else {
                ImGui::TextColored(COL_RED, "Server: Stopped");
            }
            ImVec4 tpsCol = srv.averageTPS >= 19 ? COL_GREEN : (srv.averageTPS >= 15 ? COL_YELLOW : COL_RED);
            ImGui::TextColored(tpsCol, "TPS: %.1f / 20", srv.averageTPS);
            ImGui::Text("Avg Tick Time:  %.2f ms", srv.averageTickTime);
            ImGui::Text("Ticks Total:    %llu", (unsigned long long)srv.ticksProcessed);
            ImGui::Separator();
            ImGui::Text("Chunks Loaded:  %llu", (unsigned long long)srv.chunksLoaded);
            ImGui::Text("Chunks Sent:    %llu", (unsigned long long)srv.chunksSent);
            ImGui::Text("Block Changes:  %llu", (unsigned long long)srv.blockChangesProcessed);
            ImGui::Text("Packets In/Out: %llu / %llu",
                (unsigned long long)srv.packetsReceived, (unsigned long long)srv.packetsSent);
        }

        // Server workers
        if (ImGui::CollapsingHeader("Server Workers", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Workers: %zu  |  Pending: %zu  |  Active: %zu",
                srv.serverWorkerCount, srv.serverPendingJobs, srv.serverActiveJobs);
            ImGui::Separator();
            ImGui::Text("Generated:  %zu", srv.serverChunksGenerated);
            ImGui::Text("Loaded:     %zu", srv.serverChunksLoaded);
            ImGui::Text("Saved:      %zu", srv.serverChunksSaved);
            ImGui::Text("Jobs: %zu submitted, %zu completed", srv.serverJobsSubmitted, srv.serverJobsCompleted);
            if (srv.serverJobsCancelled > 0)
                ImGui::TextColored(COL_YELLOW, "Cancelled: %zu", srv.serverJobsCancelled);
            if (srv.serverJobsFailed > 0)
                ImGui::TextColored(COL_RED, "Failed: %zu", srv.serverJobsFailed);
        }

        // Network
        if (ImGui::CollapsingHeader("Network", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (net.connected) {
                ImGui::TextColored(COL_GREEN, "Connected");
            } else {
                ImGui::TextColored(COL_RED, "Disconnected");
            }

            char sentBuf[32], recvBuf[32];
            FormatBytes(net.bytesSent, sentBuf, sizeof(sentBuf));
            FormatBytes(net.bytesReceived, recvBuf, sizeof(recvBuf));

            ImGui::Text("Sent:     %s (%llu packets)", sentBuf, (unsigned long long)net.packetsSent);
            ImGui::Text("Received: %s (%llu packets)", recvBuf, (unsigned long long)net.packetsReceived);
            ImGui::Text("Queue:    %zu pending", net.incomingQueueSize);
            if (net.droppedPacketCount > 0)
                ImGui::TextColored(COL_RED, "Dropped: %zu packets!", net.droppedPacketCount);

            int upMin = (int)(net.connectionUptimeSec / 60.0f);
            int upSec = (int)fmodf(net.connectionUptimeSec, 60.0f);
            ImGui::Text("Uptime:   %dm %ds", upMin, upSec);
        }

        ImGui::End();
    }

    // ========================================================================
    // CLIENT SYSTEMS PANEL
    // ========================================================================

    void DebugSystem::DrawClientSystemsPanel() {
        ImGui::SetNextWindowPos(ImVec2(440, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Client Systems", &s_visibility.clientSystems)) {
            ImGui::End();
            return;
        }

        // Client Chunks
        if (ImGui::CollapsingHeader("Client Chunks", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (Client::g_clientChunkManager) {
                size_t loaded = Client::g_clientChunkManager->GetLoadedChunkCount();
                ImGui::Text("Loaded Chunks: %zu", loaded);

                size_t total = 0, ready = 0, meshing = 0, dirty = 0;
                Client::g_clientChunkManager->GetSectionStats(total, ready, meshing, dirty);
                ImGui::Text("Sections: %zu total", total);
                ImGui::TextColored(COL_GREEN,  "  Ready:   %zu", ready);
                ImGui::TextColored(COL_YELLOW, "  Meshing: %zu", meshing);
                ImGui::TextColored(COL_ORANGE, "  Dirty:   %zu", dirty);
            } else {
                ImGui::TextDisabled("ClientChunkManager unavailable");
            }
        }

        // Client Workers
        if (ImGui::CollapsingHeader("Client Workers", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (Threading::g_clientWorkerPool) {
                size_t pending = Threading::g_clientWorkerPool->GetPendingJobCount();
                size_t active = Threading::g_clientWorkerPool->GetActiveJobCount();
                ImGui::Text("Workers: %zu  |  Pending: %zu  |  Active: %zu",
                    Threading::g_clientWorkerPool->GetWorkerCount(), pending, active);

                const auto& ws = Threading::g_clientWorkerPool->GetStats();
                ImGui::Text("Mesh Jobs: %zu submitted, %zu completed",
                    ws.meshJobsSubmitted.load(std::memory_order_relaxed),
                    ws.meshJobsCompleted.load(std::memory_order_relaxed));
                ImGui::Text("Sections Built: %zu", ws.sectionsBuilt.load(std::memory_order_relaxed));
                ImGui::Text("Verts: %zu  Indices: %zu",
                    ws.verticesGenerated.load(std::memory_order_relaxed),
                    ws.indicesGenerated.load(std::memory_order_relaxed));
                size_t cancelled = ws.meshJobsCancelled.load(std::memory_order_relaxed);
                size_t failed = ws.meshJobsFailed.load(std::memory_order_relaxed);
                if (cancelled > 0) ImGui::TextColored(COL_YELLOW, "Cancelled: %zu", cancelled);
                if (failed > 0) ImGui::TextColored(COL_RED, "Failed: %zu", failed);
            } else {
                ImGui::TextDisabled("ClientWorkerPool unavailable");
            }
        }

        // Mesh Manager
        if (ImGui::CollapsingHeader("Mesh Manager")) {
            if (Render::g_clientMeshManager) {
                const auto& ms = Render::g_clientMeshManager->GetStats();
                ImGui::Text("Scheduled:  %zu", ms.meshBuildsScheduled.load(std::memory_order_relaxed));
                ImGui::Text("Completed:  %zu", ms.meshBuildsCompleted.load(std::memory_order_relaxed));
                ImGui::Text("Uploaded:   %zu", ms.meshUploadedToGPU.load(std::memory_order_relaxed));
                ImGui::Text("Cancelled:  %zu", ms.meshBuildsCancelled.load(std::memory_order_relaxed));
                ImGui::Text("Skipped:    %zu", ms.meshBuildsSkipped.load(std::memory_order_relaxed));
                ImGui::Separator();
                ImGui::Text("This Frame: %d builds, %d uploads", ms.meshBuildsThisFrame, ms.meshUploadsThisFrame);
            } else {
                ImGui::TextDisabled("ClientMeshManager unavailable");
            }
        }

        // GPU Data Pool
        if (ImGui::CollapsingHeader("GPU Data Pool")) {
            if (Render::g_gpuDataPool) {
                size_t poolSize = Render::g_gpuDataPool->getPoolSize();
                size_t inUse = Render::g_gpuDataPool->getInUseCount();
                size_t deferred = Render::g_gpuDataPool->getDeferredCount();
                size_t total = poolSize + inUse;

                ImGui::Text("In Use:   %zu / %zu", inUse, total);
                if (total > 0) {
                    float utilization = (float)inUse / (float)total;
                    ImGui::ProgressBar(utilization, ImVec2(-1, 0),
                        (std::to_string(inUse) + "/" + std::to_string(total)).c_str());
                }
                ImGui::Text("Available: %zu  Deferred: %zu", poolSize, deferred);
            } else {
                ImGui::TextDisabled("GPUDataPool unavailable");
            }
        }

        ImGui::End();
    }

    // ========================================================================
    // MEMORY PANEL
    // ========================================================================

    void DebugSystem::DrawMemoryPanel(const PerformanceMetrics& metrics) {
        ImGui::SetNextWindowPos(ImVec2(830, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 250), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Memory", &s_visibility.memory)) {
            ImGui::End();
            return;
        }

        // Render backend info
        if (Render::g_renderBackend) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Backend: %s", Render::g_renderBackend->GetName());
            auto memStats = Render::g_renderBackend->GetMemoryStats();
            ImGui::Text("GPU Total: %.2f MB (peak %.2f MB)",
                       memStats.totalAllocated / (1024.0f * 1024.0f),
                       memStats.peakUsage / (1024.0f * 1024.0f));
            ImGui::Text("  Buffers: %.2f MB (%zu)", memStats.bufferMemory / (1024.0f * 1024.0f), memStats.bufferCount);
            ImGui::Text("  Textures: %.2f MB (%zu)", memStats.textureMemory / (1024.0f * 1024.0f), memStats.textureCount);
            ImGui::Text("  Meshes: %zu  Shaders: %zu", memStats.meshCount, memStats.shaderCount);
        } else {
            ImGui::Text("Backend: Direct OpenGL (no abstraction)");
        }
        ImGui::Separator();

        // GPU memory estimate (approximate: indices->quads->vertices)
        size_t estVertices = (metrics.totalIndicesRendered / 6) * 4;
        float gpuMemMB = (float)(estVertices * sizeof(Render::Vertex) +
                                 metrics.totalIndicesRendered * sizeof(uint32_t)) / (1024.0f * 1024.0f);
        ImGui::Text("~GPU Mesh Memory: %.2f MB", gpuMemMB);
        ImGui::Text("  ~Vertices: %zu  Indices: %zu", estVertices, metrics.totalIndicesRendered);

        ImGui::Separator();

        // GPU Pool
        if (Render::g_gpuDataPool) {
            size_t inUse = Render::g_gpuDataPool->getInUseCount();
            size_t poolSz = Render::g_gpuDataPool->getPoolSize();
            ImGui::Text("GPU Pool: %zu in use, %zu available", inUse, poolSz);
        }

        ImGui::Separator();

        // World
        if (Game::g_world) {
            ImGui::Text("World Loaded Chunks: %zu", Game::g_world->GetLoadedChunkCount());
            ImGui::Text("Render Distance: %d", Game::g_world->GetRenderDistance());
        }

        ImGui::End();
    }

    // ========================================================================
    // PLAYER PANEL
    // ========================================================================

    void DebugSystem::DrawPlayerPanel(Game::ClientPlayer& player, Game::ClientPlayerController& playerController, const Render::Camera& camera) {
        ImGui::SetNextWindowPos(ImVec2(830, 370), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Player", &s_visibility.player)) {
            ImGui::End();
            return;
        }

        const auto& phys = player.physics;

        // Position
        if (ImGui::CollapsingHeader("Position & Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Physics:   (%.2f, %.2f, %.2f)", phys.position.x, phys.position.y, phys.position.z);
            ImGui::Text("Server:    (%.2f, %.2f, %.2f)", player.serverPos.x, player.serverPos.y, player.serverPos.z);
            ImGui::Text("Visual:    (%.2f, %.2f, %.2f)", player.visualPos.x, player.visualPos.y, player.visualPos.z);
            ImGui::Separator();
            ImGui::Text("Yaw: %.1f  Pitch: %.1f", camera.yaw, camera.pitch);
            ImGui::Text("Facing: %s", GetDirectionName(camera.yaw));

            int cx = (int)std::floor(phys.position.x / 16.0f);
            int cz = (int)std::floor(phys.position.z / 16.0f);
            int sy = (int)std::floor((phys.position.y + 64.0f) / 16.0f);
            ImGui::Text("Chunk: (%d, %d)  Section: %d", cx, cz, sy);
        }

        // Physics
        if (ImGui::CollapsingHeader("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Velocity:  (%.2f, %.2f, %.2f)", phys.velocity.x, phys.velocity.y, phys.velocity.z);
            ImGui::Text("Speed:     %.2f blocks/s", glm::length(phys.velocity));
            ImGui::TextColored(phys.isOnGround ? COL_GREEN : COL_GRAY, "Ground: %s", phys.isOnGround ? "YES" : "NO");
            ImGui::SameLine();
            ImGui::TextColored(phys.isInWater ? COL_BLUE : COL_GRAY, "Water: %s", phys.isInWater ? "YES" : "NO");
            ImGui::TextColored(phys.isSneaking ? COL_YELLOW : COL_GRAY, "Sneak: %s", phys.isSneaking ? "YES" : "NO");
            ImGui::SameLine();
            ImGui::TextColored(phys.isSprinting ? COL_GREEN : COL_GRAY, "Sprint: %s", phys.isSprinting ? "YES" : "NO");
            ImGui::SameLine();
            ImGui::TextColored(phys.noclip ? COL_BLUE : COL_GRAY, "Noclip: %s", phys.noclip ? "ON" : "OFF");
        }

        // Attributes
        if (ImGui::CollapsingHeader("Attributes")) {
            ImGui::Text("Health: %d/20  Food: %d/20  Air: %d", player.health, player.food, player.air);
        }

        // Interaction
        if (ImGui::CollapsingHeader("Interaction", ImGuiTreeNodeFlags_DefaultOpen)) {
            Game::BlockID selected = player.inventory.GetSelectedBlock();
            ImGui::Text("Selected: %s (Slot %d)",
                selected == Game::BlockID::Air ? "None" : Game::BlockRegistry::Get(selected).name.c_str(),
                player.inventory.GetSelectedSlot());

            const auto& hit = player.lastBlockHit;
            if (hit.has_value()) {
                ImGui::Text("Looking at: %s at (%d, %d, %d)",
                    Game::BlockRegistry::Get(hit->blockId).name.c_str(),
                    hit->blockPos.x, hit->blockPos.y, hit->blockPos.z);
                ImGui::Text("Distance: %.2f", hit->distance);

                if (playerController.IsBreaking()) {
                    float prog = playerController.GetBreakProgress();
                    ImGui::Text("Breaking: %.0f%%", prog * 100.0f);
                    ImGui::ProgressBar(prog, ImVec2(-1, 0));
                }
            } else {
                ImGui::TextColored(COL_GRAY, "Looking at: Nothing");
            }
        }

        // Stats
        if (ImGui::CollapsingHeader("Statistics")) {
            const auto& st = player.stats;
            ImGui::Text("Blocks Placed:  %d", st.blocksPlaced);
            ImGui::Text("Blocks Broken:  %d", st.blocksBroken);
            ImGui::Text("Distance:       %.1f blocks", st.totalDistanceTraveled);

            int mins = (int)(st.totalPlayTime / 60.0f);
            int secs = (int)fmodf(st.totalPlayTime, 60.0f);
            ImGui::Text("Play Time:      %dm %ds", mins, secs);
        }

        // Noclip controls
        if (ImGui::CollapsingHeader("Noclip Flight Controls")) {
            auto& p = player.physics;
            if (p.noclip) {
                ImGui::TextColored(COL_GREEN, "Noclip Active");
            } else {
                ImGui::TextColored(COL_GRAY, "Noclip Disabled (press N)");
            }

            ImGui::Text("Horizontal Speed:");
            ImGui::SliderFloat("##HSpeed", &p.noclipHorizontalSpeed, 1.0f, 50.0f, "%.1f blocks/s");
            ImGui::Text("Vertical Speed:");
            ImGui::SliderFloat("##VSpeed", &p.noclipVerticalSpeed, 1.0f, 50.0f, "%.1f blocks/s");

            if (ImGui::Button("Reset")) {
                p.noclipHorizontalSpeed = Game::PlayerPhysics::NOCLIP_HORIZONTAL_SPEED;
                p.noclipVerticalSpeed = Game::PlayerPhysics::NOCLIP_VERTICAL_SPEED;
            }
            ImGui::SameLine();
            if (ImGui::Button("Fast (25)")) {
                p.noclipHorizontalSpeed = 25.0f; p.noclipVerticalSpeed = 25.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Slow (5)")) {
                p.noclipHorizontalSpeed = 5.0f; p.noclipVerticalSpeed = 5.0f;
            }
        }

        ImGui::End();
    }

    // ========================================================================
    // RENDER CONTROLS PANEL
    // ========================================================================

    void DebugSystem::DrawRenderControlsPanel(int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight) {
        ImGui::SetNextWindowPos(ImVec2(830, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 400), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Render Controls", &s_visibility.renderControls)) {
            ImGui::End();
            return;
        }

        // VSync
        static bool vsyncEnabled = true;
        if (ImGui::Checkbox("VSync", &vsyncEnabled)) {
            if (Render::g_renderBackend) {
                Render::g_renderBackend->SetVSync(vsyncEnabled);
            } else {
                glfwSwapInterval(vsyncEnabled ? 1 : 0);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Sync frame rate with monitor refresh");

        // Rendering mode
        ImGui::Separator();
        static bool useMinecraftRendering = false;
        if (ImGui::Checkbox("Minecraft-Style Rendering", &useMinecraftRendering)) {
            ApplyRenderingMode(useMinecraftRendering);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Minecraft-Style: GL_NEAREST_MIPMAP_LINEAR, border extrusion, sRGB\n"
                "Classic: GL_LINEAR_MIPMAP_LINEAR, standard RGBA"
            );
        }

        // Mipmap level
        if (useMinecraftRendering && Render::g_atlasBuilder) {
            int mipmapLevel = Render::g_atlasBuilder->GetMipmapLevel();
            if (ImGui::SliderInt("Mipmap Level", &mipmapLevel, 0, 4)) {
                Render::g_atlasBuilder->SetMipmapLevel(mipmapLevel);
            }
        }

        // Debug rendering controls
        ImGui::Separator();
        ImGui::TextColored(COL_BLUE, "Debug Rendering");

        if (Render::g_chunkRenderer) {
            static bool wireframe = false;
            if (ImGui::Checkbox("Wireframe", &wireframe)) {
                Render::g_chunkRenderer->SetWireframeMode(wireframe);
            }

            bool frustumCulling = Render::g_chunkRenderer->IsEnabledFrustumCulling();
            if (ImGui::Checkbox("Frustum Culling", &frustumCulling)) {
                Render::g_chunkRenderer->SetEnableFrustumCulling(frustumCulling);
            }

            static int debugLayer = -1;
            const char* layerNames[] = {"All Layers", "Opaque Only", "Cutout Only", "Translucent Only"};
            if (ImGui::Combo("Layer", &debugLayer, layerNames, 4)) {
                Render::g_chunkRenderer->SetDebugLayer(debugLayer - 1); // -1=all, 0=opaque, 1=cutout, 2=translucent
            }
        }

        // Crosshair
        ImGui::Separator();
        ImGui::TextColored(COL_BLUE, "Crosshair");
        bool crosshairVisible = Render::g_crosshair.IsVisible();
        if (ImGui::Checkbox("Show Crosshair", &crosshairVisible)) {
            Render::g_crosshair.SetVisible(crosshairVisible);
        }
        int crosshairSize = Render::g_crosshair.GetSize();
        if (ImGui::SliderInt("Size", &crosshairSize, 2, 100)) {
            Render::g_crosshair.SetSize(crosshairSize);
        }

        // Display info
        ImGui::Separator();
        ImGui::TextColored(COL_BLUE, "Display");
        ImGui::Text("Window:      %dx%d", windowWidth, windowHeight);
        ImGui::Text("Framebuffer: %dx%d", framebufferWidth, framebufferHeight);

        float xscale = (float)framebufferWidth / (float)windowWidth;
        float yscale = (float)framebufferHeight / (float)windowHeight;
        ImGui::Text("Scale:       %.2fx%.2f", xscale, yscale);

        if (framebufferWidth != windowWidth || framebufferHeight != windowHeight) {
            ImGui::TextColored(COL_GREEN, "Retina/High-DPI Active");
        }

        ImGui::End();
    }

    // ========================================================================
    // CHUNK VISUALIZATION
    // ========================================================================

    bool DebugSystem::IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos) {
        float worldX = static_cast<float>(chunkPos.x * Game::Math::CHUNK_SIZE_X);
        float worldZ = static_cast<float>(chunkPos.z * Game::Math::CHUNK_SIZE_Z);
        AABB chunkAABB;
        chunkAABB.min = glm::vec3(worldX, Config::MinY, worldZ);
        chunkAABB.max = glm::vec3(worldX + Game::Math::CHUNK_SIZE_X, Config::MaxY, worldZ + Game::Math::CHUNK_SIZE_Z);
        return frustum.IsBoxVisible(chunkAABB);
    }

    void DebugSystem::DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum) {
        ImGui::SetNextWindowPos(ImVec2(10, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(520, 540), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Chunk Map", &s_visibility.chunkViz)) {
            ImGui::End();
            return;
        }

        int cameraChunkX = (int)std::floor(camera.position.x / Game::Math::CHUNK_SIZE_X);
        int cameraChunkZ = (int)std::floor(camera.position.z / Game::Math::CHUNK_SIZE_Z);

        const int vizRadius = 12;
        const float circleRadius = 8.0f;
        const float spacing = 20.0f;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 500) canvasSize.x = 500;
        if (canvasSize.y < 500) canvasSize.y = 500;

        ImVec2 center(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);

        // Grid
        ImU32 gridColor = IM_COL32(64, 64, 64, 128);
        for (int i = -vizRadius; i <= vizRadius; i++) {
            float x = center.x + i * spacing;
            drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), gridColor);
            float y = center.y + i * spacing;
            drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), gridColor);
        }

        int totalChunks = 0, generatedChunks = 0, visibleChunks = 0;

        for (int dz = -vizRadius; dz <= vizRadius; dz++) {
            for (int dx = -vizRadius; dx <= vizRadius; dx++) {
                totalChunks++;
                Game::Math::ChunkPos cp = {cameraChunkX + dx, cameraChunkZ + dz};

                bool isGenerated = Game::g_world && Game::g_world->IsChunkLoaded(cp.x, cp.z);
                bool inFrustum = false;
                if (isGenerated) {
                    generatedChunks++;
                    inFrustum = IsChunkInFrustum(frustum, cp);
                    if (inFrustum) visibleChunks++;
                }

                ImU32 col = !isGenerated ? IM_COL32(255, 64, 64, 255) :
                            inFrustum ? IM_COL32(64, 255, 64, 255) :
                            IM_COL32(255, 255, 64, 255);

                ImVec2 pos(center.x + dx * spacing, center.y + dz * spacing);
                drawList->AddCircleFilled(pos, circleRadius, col);
                drawList->AddCircle(pos, circleRadius, IM_COL32(128, 128, 128, 255), 0, 1.5f);

                if (ImGui::IsMouseHoveringRect(
                    ImVec2(pos.x - circleRadius, pos.y - circleRadius),
                    ImVec2(pos.x + circleRadius, pos.y + circleRadius))) {
                    ImGui::SetTooltip("Chunk (%d, %d)\nLoaded: %s\nVisible: %s",
                        cp.x, cp.z, isGenerated ? "Yes" : "No", inFrustum ? "Yes" : "No");
                }
            }
        }

        // Player
        drawList->AddCircleFilled(center, circleRadius + 2, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(center, circleRadius + 2, IM_COL32(0, 0, 0, 255), 0, 3.0f);

        float yawRad = glm::radians(camera.yaw);
        ImVec2 dirEnd(center.x + cos(yawRad) * (circleRadius + 8), center.y + sin(yawRad) * (circleRadius + 8));
        drawList->AddLine(center, dirEnd, IM_COL32(0, 0, 0, 255), 3.0f);

        ImGui::InvisibleButton("chunk_viz_canvas", canvasSize);

        ImGui::Separator();
        ImGui::Text("Total: %d  Loaded: %d  Visible: %d  Camera: (%d, %d)",
            totalChunks, generatedChunks, visibleChunks, cameraChunkX, cameraChunkZ);

        ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "Red: Unloaded");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0.25f, 1), "Yellow: Loaded (not visible)");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.25f, 1, 0.25f, 1), "Green: Visible");

        ImGui::End();
    }

    // ========================================================================
    // TEXTURE ATLAS DEBUG
    // ========================================================================

    void DebugSystem::DrawTextureAtlasDebug() {
        ImGui::SetNextWindowPos(ImVec2(540, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(500, 540), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Texture Atlas", &s_visibility.textureAtlas)) {
            ImGui::End();
            return;
        }

        if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
            DrawAtlasBuilderDebug();
        } else {
            ImGui::TextDisabled("Texture atlas not initialized");
        }

        ImGui::End();
    }

    void DebugSystem::DrawAtlasBuilderDebug() {
        auto& ab = *Render::g_atlasBuilder;

        ImGui::Text("Size: %dx%d  Textures: %zu  ID: %lu",
            ab.GetAtlasWidth(), ab.GetAtlasHeight(), ab.GetTextureCount(), ab.GetAtlasTextureID());

        ImGui::Separator();

        static float zoomLevel = 1.0f;
        ImGui::SliderFloat("Zoom", &zoomLevel, 0.25f, 10.0f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::Button("Reset##zoom")) zoomLevel = 1.0f;

        float aspect = (float)ab.GetAtlasHeight() / (float)ab.GetAtlasWidth();
        float baseW = 400.0f;
        float dispW = baseW * zoomLevel;
        float dispH = baseW * aspect * zoomLevel;

        ImGui::BeginChild("AtlasScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImTextureID texID = (ImTextureID)(uintptr_t)ab.GetAtlasTextureID();
        ImGui::Image(texID, ImVec2(dispW, dispH));

        if (ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
            ImVec2 mp = ImGui::GetMousePos();
            if (mp.x >= cursorPos.x && mp.x <= cursorPos.x + dispW &&
                mp.y >= cursorPos.y && mp.y <= cursorPos.y + dispH) {
                float u = std::clamp((mp.x - cursorPos.x) / dispW, 0.0f, 1.0f);
                float v = std::clamp((mp.y - cursorPos.y) / dispH, 0.0f, 1.0f);
                int px = (int)(u * ab.GetAtlasWidth());
                int py = (int)(v * ab.GetAtlasHeight());
                ImGui::SetTooltip("UV: (%.3f, %.3f)\nPixel: (%d, %d)", u, v, px, py);

                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 xhCol = IM_COL32(255, 0, 0, 200);
                dl->AddLine(ImVec2(mp.x, cursorPos.y), ImVec2(mp.x, cursorPos.y + dispH), xhCol);
                dl->AddLine(ImVec2(cursorPos.x, mp.y), ImVec2(cursorPos.x + dispW, mp.y), xhCol);
            }
        }

        ImGui::EndChild();
    }

    // ========================================================================
    // WORLD DEBUG (kept for load controls)
    // ========================================================================

    void DebugSystem::DrawWorldDebug() {
        // World debug is now integrated into other panels
        // This function exists for backwards compatibility but is no longer called
    }

    // ========================================================================
    // LOG CONSOLE
    // ========================================================================

    void DebugSystem::DrawLogConsolePanel() {
        ImGui::SetNextWindowPos(ImVec2(10, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700, 300), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Log Console", &s_visibility.logConsole)) {
            ImGui::End();
            return;
        }

        // Controls
        static bool showDebug = true, showInfo = true, showWarning = true, showError = true;
        static bool autoScroll = true;
        static char filterBuf[128] = "";

        ImGui::Checkbox("DEBUG", &showDebug); ImGui::SameLine();
        ImGui::Checkbox("INFO", &showInfo); ImGui::SameLine();
        ImGui::Checkbox("WARN", &showWarning); ImGui::SameLine();
        ImGui::Checkbox("ERROR", &showError); ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoScroll); ImGui::SameLine();
        if (ImGui::Button("Clear")) s_logBuffer.Clear();

        ImGui::InputText("Filter", filterBuf, sizeof(filterBuf));

        ImGui::Separator();

        // Log entries
        ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

        auto entries = s_logBuffer.GetEntries();
        std::string filterStr(filterBuf);

        for (const auto& entry : entries) {
            // Level filter
            switch (entry.level) {
                case Log::Level::Debug:   if (!showDebug) continue; break;
                case Log::Level::Info:    if (!showInfo) continue; break;
                case Log::Level::Warning: if (!showWarning) continue; break;
                case Log::Level::Error:   if (!showError) continue; break;
            }

            // Text filter
            if (!filterStr.empty() && entry.message.find(filterStr) == std::string::npos)
                continue;

            ImVec4 col;
            const char* prefix;
            switch (entry.level) {
                case Log::Level::Debug:   col = COL_BLUE;   prefix = "[DBG]"; break;
                case Log::Level::Info:    col = COL_GREEN;  prefix = "[INF]"; break;
                case Log::Level::Warning: col = COL_YELLOW; prefix = "[WRN]"; break;
                case Log::Level::Error:   col = COL_RED;    prefix = "[ERR]"; break;
                default:                  col = COL_WHITE;  prefix = "[???]"; break;
            }

            char timeBuf[16];
            int mins = (int)(entry.timestamp / 60.0f);
            float secs = fmodf(entry.timestamp, 60.0f);
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%05.2f", mins, secs);

            ImGui::TextColored(COL_GRAY, "%s", timeBuf);
            ImGui::SameLine();
            ImGui::TextColored(col, "%s %s", prefix, entry.message.c_str());
        }

        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
        ImGui::End();
    }

    // ========================================================================
    // CONTROLS PANEL
    // ========================================================================

    void DebugSystem::DrawControlsPanel(bool cursorEnabled, const Render::Camera& camera) {
        ImGui::SetNextWindowPos(ImVec2(1200, 110), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260, 320), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Controls", &s_visibility.controls)) {
            ImGui::End();
            return;
        }

        ImGui::TextColored(COL_BLUE, "Keybindings");
        ImGui::Separator();
        ImGui::Text("WASD        Move");
        ImGui::Text("Space       Jump / Fly Up");
        ImGui::Text("Shift       Sneak / Fly Down");
        ImGui::Text("Ctrl        Sprint");
        ImGui::Text("Mouse       Look around");
        ImGui::Text("LMB         Break block");
        ImGui::Text("RMB         Place block");
        ImGui::Text("1-9         Select hotbar slot");
        ImGui::Text("Scroll      Cycle hotbar");
        ImGui::Text("Tab         Toggle cursor");
        ImGui::Text("N           Toggle noclip");
        ImGui::Text("F3          Toggle F3 overlay");
        ImGui::Text("~           Toggle log console");
        ImGui::Text("Shift+`+D   Toggle debug UI");
        ImGui::Text("Escape      Exit");

        ImGui::Separator();
        if (cursorEnabled) {
            ImGui::TextColored(COL_RED, "Cursor: VISIBLE (Camera locked)");
        } else {
            ImGui::TextColored(COL_GREEN, "Cursor: HIDDEN (Camera active)");
        }
        ImGui::Text("Mouse Look: %s", camera.enableMouseLook ? "ON" : "OFF");

        ImGui::End();
    }

    // ========================================================================
    // APPLY RENDERING MODE
    // ========================================================================

    void DebugSystem::ApplyRenderingMode(bool useMinecraftStyle) {
        if (!Render::g_atlasBuilder || Render::g_atlasBuilder->GetAtlasTextureID() == 0) {
            Log::Warning("Cannot apply rendering mode: Atlas not available");
            return;
        }
        Render::g_atlasBuilder->RebuildAtlas(useMinecraftStyle);
    }


} // namespace Debug
