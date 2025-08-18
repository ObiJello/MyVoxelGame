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
#include "../../world/ClientWorkerPool.hpp"
#include <unordered_set>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Debug {

    void PerformanceMetrics::AddFrameTimeSample(float time) {
        frameTimes[sampleIndex] = time;
        sampleIndex = (sampleIndex + 1) % SAMPLE_COUNT;
    }

    float PerformanceMetrics::GetAverageFrameTime() const {
        float sum = 0.0f;
        for (int i = 0; i < SAMPLE_COUNT; ++i) {
            sum += frameTimes[i];
        }
        return sum / SAMPLE_COUNT;
    }

    float PerformanceMetrics::GetFPS() const {
        float avgTime = GetAverageFrameTime();
        return avgTime > 0.0f ? 1.0f / avgTime : 0.0f;
    }

#ifndef NDEBUG
    bool DebugSystem::Initialize(GLFWwindow* window) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");

        Log::Info("Debug system initialized");
        return true;
    }

    void DebugSystem::Shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        Log::Info("Debug system shutdown");
    }

    void DebugSystem::BeginFrame() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void DebugSystem::EndFrame() {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    void DebugSystem::RenderDebugUI(
        const Render::Camera& camera,
        const Frustum& frustum,
        Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {

        DrawMainDebugWindow(camera, playerController, metrics, cursorEnabled,
                           windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        DrawChunkVisualization(camera, frustum);
        DrawTextureAtlasDebug();
        DrawWorldDebug();
        DrawPlayerFlightControls(playerController);
    }

    void DebugSystem::DrawMainDebugWindow(
        const Render::Camera& camera,
        Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {

        ImGui::Begin("Voxel Engine Debug");

        // Performance metrics with detailed breakdown
        ImGui::Text("Performance Breakdown");
        ImGui::Separator();
        ImGui::Text("FPS: %.1f (Avg: %.2f ms)", metrics.GetFPS(), metrics.GetAverageFrameTime() * 1000.0f);
        ImGui::Text("Frame Time: %.2f ms", metrics.frameTime);
        
        // CPU Operations
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "=== CPU Operations ===");
        ImGui::Indent();
        
        // Calculate percentages for better context
        auto pct = [&metrics](float time) { 
            return metrics.frameTime > 0 ? (time / metrics.frameTime * 100.0f) : 0.0f; 
        };
        
        ImGui::Text("Network Processing: %.2f ms (%.1f%%)", metrics.networkProcessingTime, pct(metrics.networkProcessingTime));
        ImGui::Text("Input Handling: %.2f ms (%.1f%%)", metrics.inputHandlingTime, pct(metrics.inputHandlingTime));
        ImGui::Text("Game Logic: %.2f ms (%.1f%%)", metrics.gameLogicTime, pct(metrics.gameLogicTime));
        ImGui::Text("Mesh Results Processing: %.2f ms (%.1f%%)", metrics.meshResultProcessingTime, pct(metrics.meshResultProcessingTime));
        ImGui::Text("Mesh Scheduling: %.2f ms (%.1f%%)", metrics.meshSchedulingTime, pct(metrics.meshSchedulingTime));
        ImGui::Text("Texture Animation: %.2f ms (%.1f%%)", metrics.textureAnimationTime, pct(metrics.textureAnimationTime));
        ImGui::Unindent();
        
        // GPU Operations
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "=== GPU Operations ===");
        ImGui::Indent();
        ImGui::Text("GPU Upload: %.2f ms (%.1f%%)", metrics.gpuUploadTime, pct(metrics.gpuUploadTime));
        ImGui::Text("Rendering: %.2f ms (%.1f%%)", metrics.renderTime, pct(metrics.renderTime));
        
        // Collapsible render timing breakdown
        if (ImGui::TreeNode("Render Breakdown")) {
            // Get detailed render stats from ChunkRenderer
            if (auto* renderStats = Render::GetChunkRendererStats()) {
                ImGui::Indent();
                
                // === LOCK-FREE RENDERING STATUS ===
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "🚀 Lock-free Rendering: ENABLED");
                ImGui::Separator();
                
                // === BUILD PHASE (NEW SYSTEM) ===
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.5f, 1.0f), "Build Draw Lists: %.2f ms", renderStats->buildDrawListsTimeMs);
                if (ImGui::TreeNode("Build Phase Details")) {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "Chunk Iteration: %.2f ms", 
                                      renderStats->chunkIterationTimeMs);
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "GPU Data Load (atomic): %.2f ms", 
                                      renderStats->gpuDataLoadTimeMs);
                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Frustum Culling: %.2f ms", 
                                      renderStats->frustumCullingTimeMs);
                    ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Sorting: %.2f ms", 
                                      renderStats->sortingTimeMs);
                    
                    // Show sections info
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sections Checked: %d", renderStats->sectionsAvailable);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sections Culled: %d", renderStats->sectionsSkipped);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sections Rendered: %d", renderStats->sectionsRendered);
                    ImGui::Unindent();
                    ImGui::TreePop();
                }
                
                ImGui::Spacing();
                
                // === RENDER PASSES ===
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Opaque Pass: %.2f ms (%d sections)", 
                                  renderStats->opaquePassTimeMs, renderStats->opaqueSections);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.8f, 1.0f), "Cutout Pass: %.2f ms (%d sections)", 
                                  renderStats->cutoutPassTimeMs, renderStats->cutoutSections);
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "Translucent Pass: %.2f ms (%d sections)", 
                                  renderStats->translucentPassTimeMs, renderStats->translucentSections);
                
                // Total draw calls are still tracked
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.8f, 1.0f), "Total Draw Calls: %d", 
                                  renderStats->totalDrawCalls);
                
                ImGui::Spacing();
                
                // === SUMMARY ===
                float renderPassesTotal = renderStats->opaquePassTimeMs + renderStats->cutoutPassTimeMs + 
                                         renderStats->translucentPassTimeMs;
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Build + Render Passes: %.2f ms", 
                                  renderStats->buildDrawListsTimeMs + renderPassesTotal);
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Total Render Time: %.2f ms", 
                                  renderStats->renderTimeMs);
                
                // Highlight potential issues with NEW system
                if (renderStats->gpuDataLoadTimeMs > 1.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 
                                      "⚠ Atomic loads taking longer than expected!");
                }
                if (renderStats->buildDrawListsTimeMs > 10.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 
                                      "⚠ Draw list building taking too long!");
                }
                
                ImGui::Unindent();
            } else {
                ImGui::TextDisabled("Render stats not available");
            }
            ImGui::TreePop();
        }
        ImGui::Unindent();
        
        // System Operations
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "=== System Operations ===");
        ImGui::Indent();
        ImGui::Text("Debug UI: %.2f ms (%.1f%%)", metrics.debugUITime, pct(metrics.debugUITime));
        
        // VSync with color coding
        if (metrics.vsyncWaitTime > 10.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "VSync Wait: %.2f ms (%.1f%%) ⚠", 
                              metrics.vsyncWaitTime, pct(metrics.vsyncWaitTime));
        } else if (metrics.vsyncWaitTime > 5.0f) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "VSync Wait: %.2f ms (%.1f%%)", 
                              metrics.vsyncWaitTime, pct(metrics.vsyncWaitTime));
        } else {
            ImGui::Text("VSync Wait: %.2f ms (%.1f%%)", metrics.vsyncWaitTime, pct(metrics.vsyncWaitTime));
        }
        
        // Other/Unaccounted time
        if (std::abs(metrics.otherTime) > 1.0f) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "Other/Unaccounted: %.2f ms (%.1f%%)", 
                              metrics.otherTime, pct(metrics.otherTime));
        } else {
            ImGui::Text("Other/Unaccounted: %.2f ms (%.1f%%)", metrics.otherTime, pct(metrics.otherTime));
        }
        ImGui::Unindent();
        
        // Performance Analysis
        ImGui::Spacing();
        ImGui::Separator();
        
        // Find the biggest time consumer
        struct TimerInfo {
            const char* name;
            float time;
            ImVec4 color;
        };
        
        std::vector<TimerInfo> timers = {
            {"Network", metrics.networkProcessingTime, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
            {"Input", metrics.inputHandlingTime, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
            {"Game Logic", metrics.gameLogicTime, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
            {"Mesh Results", metrics.meshResultProcessingTime, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
            {"Mesh Scheduling", metrics.meshSchedulingTime, ImVec4(0.5f, 0.8f, 1.0f, 1.0f)},
            {"GPU Upload", metrics.gpuUploadTime, ImVec4(1.0f, 0.8f, 0.5f, 1.0f)},
            {"Rendering", metrics.renderTime, ImVec4(1.0f, 0.8f, 0.5f, 1.0f)},
            {"Debug UI", metrics.debugUITime, ImVec4(0.7f, 1.0f, 0.7f, 1.0f)},
            {"VSync", metrics.vsyncWaitTime, ImVec4(0.7f, 1.0f, 0.7f, 1.0f)}
        };
        
        // Find bottleneck
        auto bottleneck = std::max_element(timers.begin(), timers.end(), 
            [](const TimerInfo& a, const TimerInfo& b) { return a.time < b.time; });
        
        if (bottleneck != timers.end() && bottleneck->time > 10.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 
                              "⚠ BOTTLENECK: %s (%.2f ms / %.1f%%)", 
                              bottleneck->name, bottleneck->time, pct(bottleneck->time));
        } else if (bottleneck != timers.end() && bottleneck->time > 5.0f) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), 
                              "⚡ Slowest: %s (%.2f ms / %.1f%%)", 
                              bottleneck->name, bottleneck->time, pct(bottleneck->time));
        }
        
        ImGui::Spacing();

        // Client-Server Architecture Stats
        ImGui::Text("Client-Server System");
        ImGui::Separator();
        
        // Get stats from ClientChunkManager if available
        if (Client::g_clientChunkManager) {
            size_t loadedChunks = Client::g_clientChunkManager->GetLoadedChunkCount();
            ImGui::Text("Client Chunks Loaded: %zu", loadedChunks);
            
            // Calculate sections stats
            size_t totalSections = 0;
            size_t readySections = 0;
            size_t meshingSections = 0;
            size_t dirtySections = 0;
            Client::g_clientChunkManager->GetSectionStats(totalSections, readySections, meshingSections, dirtySections);
            
            ImGui::Text("Sections - Total: %zu, Ready: %zu", totalSections, readySections);
            ImGui::Text("Sections - Meshing: %zu, Dirty: %zu", meshingSections, dirtySections);
        } else {
            ImGui::TextDisabled("ClientChunkManager not available");
        }
        
        // Get stats from ClientMeshManager if available
        if (Render::g_clientMeshManager) {
            const auto& meshStats = Render::g_clientMeshManager->GetStats();
            ImGui::Text("Mesh Builds - Scheduled: %zu, Completed: %zu", 
                       meshStats.meshBuildsScheduled.load(), meshStats.meshBuildsCompleted.load());
            ImGui::Text("GPU Uploads This Frame: %d", meshStats.meshUploadsThisFrame);
        } else {
            ImGui::TextDisabled("ClientMeshManager not available");
        }
        
        // Get worker pool stats
        if (Threading::g_clientWorkerPool) {
            size_t pendingJobs = Threading::g_clientWorkerPool->GetPendingJobCount();
            size_t activeJobs = Threading::g_clientWorkerPool->GetActiveJobCount();
            ImGui::Text("Worker Jobs - Pending: %zu, Active: %zu", pendingJobs, activeJobs);
        } else {
            ImGui::TextDisabled("ClientWorkerPool not available");
        }
        
        ImGui::Spacing();
        
        // Mesh Rendering Stats  
        ImGui::Text("Mesh Rendering");
        ImGui::Separator();
        
        // Get frustum culling stats from ChunkRenderer
        int totalSectionsChecked = 0;
        if (auto* renderStats = Render::GetChunkRendererStats()) {
            totalSectionsChecked = renderStats->sectionsAvailable;
        }
        
        // Show rendering statistics
        ImGui::Text("Sections Rendered: %d", metrics.meshesRenderedThisFrame);
        if (totalSectionsChecked > 0) {
            ImGui::Text("Total Sections in Range: %d", totalSectionsChecked);
            // Show a simple ratio without claiming it's "culling effectiveness"
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
                              "Rendering %d of %d sections", 
                              metrics.meshesRenderedThisFrame, totalSectionsChecked);
        }
        
        ImGui::Text("By Layer - Opaque:%d, Cutout:%d, Translucent:%d", 
                   metrics.opaqueMeshesRendered,
                   metrics.cutoutMeshesRendered,
                   metrics.translucentMeshesRendered);
        ImGui::Text("Geometry - Vertices: %zu, Indices: %zu", 
                   metrics.totalVerticesRendered, metrics.totalIndicesRendered);
        
        // Memory usage estimate
        float memoryMB = (metrics.totalVerticesRendered * sizeof(Render::Vertex) +
                         metrics.totalIndicesRendered * sizeof(uint32_t)) / (1024.0f * 1024.0f);
        ImGui::Text("Estimated GPU Memory: %.2f MB", memoryMB);
        ImGui::Spacing();

        // Display Information
        ImGui::Text("Display Information");
        ImGui::Separator();
        
        // VSync toggle button
        static bool vsyncEnabled = true;  // On by default
        if (ImGui::Checkbox("VSync Enabled", &vsyncEnabled)) {
            glfwSwapInterval(vsyncEnabled ? 1 : 0);
            Log::Info("VSync %s", vsyncEnabled ? "enabled" : "disabled");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("VSync synchronizes frame rate with monitor refresh\n"
                             "Enabled: Prevents tearing, caps at monitor Hz\n"
                             "Disabled: Unlimited FPS, may cause tearing");
        }
        
        ImGui::Text("Window Size: %dx%d", windowWidth, windowHeight);
        ImGui::Text("Framebuffer Size: %dx%d", framebufferWidth, framebufferHeight);

        float xscale = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
        float yscale = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);
        ImGui::Text("Content Scale: %.2fx%.2f", xscale, yscale);

        if (framebufferWidth != windowWidth || framebufferHeight != windowHeight) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Retina/High-DPI Active");
        } else {
            ImGui::Text("Standard Display");
        }
        ImGui::Spacing();

        // Mipmap controls
        static bool mipmapEnabled = false;
        bool mipmapChanged = ImGui::Checkbox("Enable Mipmaps", &mipmapEnabled);

        if (mipmapChanged) {
            // Apply mipmap setting to AtlasBuilder
            if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
                Log::Info("Mipmap setting changed to %s", mipmapEnabled ? "enabled" : "disabled");
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mipmaps improve texture quality at distance\nbut may cause slight blurring.\n\nEnabled: GL_NEAREST_MIPMAP_LINEAR\nDisabled: GL_NEAREST");
        }

        ImGui::Spacing();
        
        // Rendering Mode Toggle
        ImGui::Separator();
        ImGui::Text("Rendering Mode:");
        static bool useMinecraftRendering = false;  // Start with classic (original) mode
        if (ImGui::Checkbox("Minecraft-Style Rendering", &useMinecraftRendering)) {
            ApplyRenderingMode(useMinecraftRendering);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Toggle between rendering modes:\n\n"
                "Minecraft-Style (NEW):\n"
                "- GL_NEAREST_MIPMAP_LINEAR filtering\n"
                "- Border extrusion to prevent bleeding\n"
                "- GL_LEQUAL depth function\n"
                "- Separate blend functions\n"
                "- sRGB texture format (GL_SRGB8_ALPHA8)\n\n"
                "Classic (OLD):\n"
                "- GL_LINEAR_MIPMAP_LINEAR filtering\n"
                "- Standard GL_LESS depth function\n"
                "- Simple blend function\n"
                "- Regular RGBA format"
            );
        }
        
        // Mipmap Level Slider (only active in Minecraft mode)
        if (useMinecraftRendering && Render::g_atlasBuilder) {
            int mipmapLevel = Render::g_atlasBuilder->GetMipmapLevel();
            if (ImGui::SliderInt("Mipmap Level", &mipmapLevel, 0, 4)) {
                Render::g_atlasBuilder->SetMipmapLevel(mipmapLevel);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("0 = No mipmaps (crisp pixels)\n1-4 = Increasing mipmap levels (smoother at distance)");
            }
        }

        ImGui::Spacing();

        // Player & Camera
        ImGui::Text("Player & Camera");
        ImGui::Separator();
        glm::vec3 camPos = camera.position;
        ImGui::Text("Position: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
        ImGui::Text("Rotation: Yaw=%.1f°, Pitch=%.1f°", camera.yaw, camera.pitch);

        // Calculate current chunk
        int cameraChunkX = static_cast<int>(std::floor(camPos.x / 16.0f));
        int cameraChunkZ = static_cast<int>(std::floor(camPos.z / 16.0f));
        int cameraSection = static_cast<int>(std::floor((camPos.y + 64.0f) / 16.0f));
        ImGui::Text("Current Chunk: (%d, %d), Section: %d", cameraChunkX, cameraChunkZ, cameraSection);
        ImGui::Spacing();

        // Physics information
        ImGui::Text("Player Physics");
        ImGui::Separator();
        const auto& playerPhysics = playerController.GetPhysics();
        ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                   playerPhysics.position.x, playerPhysics.position.y, playerPhysics.position.z);
        ImGui::Text("Velocity: (%.2f, %.2f, %.2f)",
                   playerPhysics.velocity.x, playerPhysics.velocity.y, playerPhysics.velocity.z);
        ImGui::Text("Speed: %.2f blocks/s", glm::length(playerPhysics.velocity));
        ImGui::Text("On Ground: %s", playerPhysics.isOnGround ? "YES" : "NO");
        ImGui::Text("In Water: %s", playerPhysics.isInWater ? "YES" : "NO");
        ImGui::Text("Sneaking: %s", playerPhysics.isSneaking ? "YES" : "NO");
        ImGui::Text("Sprinting: %s", playerPhysics.isSprinting ? "YES" : "NO");
        ImGui::Text("Noclip: %s", playerPhysics.noclip ? "ENABLED" : "DISABLED");

        // Player interaction info
        ImGui::Spacing();
        ImGui::Text("Player Interaction");
        ImGui::Separator();
        const auto& inventory = playerController.GetInventory();
        Game::BlockID selectedBlock = inventory.GetSelectedBlock();
        ImGui::Text("Selected Block: %s (Slot %d)",
                   selectedBlock == Game::BlockID::Air ? "None" :
                   Game::BlockRegistry::Get(selectedBlock).name.c_str(),
                   inventory.GetSelectedSlot());

        const auto& hit = playerController.GetCurrentHit();
        if (hit.has_value()) {
            ImGui::Text("Looking at: %s at (%d, %d, %d)",
                       Game::BlockRegistry::Get(hit->blockId).name.c_str(),
                       hit->blockPos.x, hit->blockPos.y, hit->blockPos.z);
            ImGui::Text("Distance: %.2f", hit->distance);

            if (playerController.IsBreaking()) {
                ImGui::Text("Breaking Progress: %.0f%%",
                           playerController.GetBreakProgress() * 100.0f);
                ImGui::ProgressBar(playerController.GetBreakProgress(), ImVec2(-1, 0));
            }
        } else {
            ImGui::Text("Looking at: Nothing");
        }

        // Crosshair settings
        ImGui::Spacing();
        ImGui::Text("Crosshair Settings");
        ImGui::Separator();
        bool crosshairVisible = Render::g_crosshair.IsVisible();
        if (ImGui::Checkbox("Show Crosshair", &crosshairVisible)) {
            Render::g_crosshair.SetVisible(crosshairVisible);
        }

        int crosshairSize = Render::g_crosshair.GetSize();
        if (ImGui::SliderInt("Crosshair Size", &crosshairSize, 2, 100)) {
            Render::g_crosshair.SetSize(crosshairSize);
        }

        // Performance graph
        ImGui::PlotLines("Frame Time (ms)", metrics.frameTimes, PerformanceMetrics::SAMPLE_COUNT,
                       metrics.sampleIndex, nullptr, 0.0f, 50.0f, ImVec2(0, 80));

        // Controls & Status
        ImGui::Spacing();
        ImGui::Text("Controls & Status");
        ImGui::Separator();
        ImGui::Text("WASD: Move horizontally");
        ImGui::Text("Space: Jump");
        ImGui::Text("Shift: Sneak");
        ImGui::Text("Ctrl: Sprint");
        ImGui::Text("Mouse: Look around (when enabled)");
        ImGui::Text("Left Click: Break blocks");
        ImGui::Text("Right Click: Place blocks");
        ImGui::Text("1-9: Select inventory slot");
        ImGui::Text("Mouse Wheel: Scroll inventory");
        ImGui::Text("Tab: Toggle cursor visibility");
        ImGui::Text("N: Toggle noclip (debug)");
        ImGui::Text("Escape: Exit");
        ImGui::Spacing();

        // Cursor status indicator
        if (cursorEnabled) {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Cursor: VISIBLE (Camera locked)");
        } else {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Cursor: HIDDEN (Camera active)");
        }
        ImGui::Text("Mouse Look: %s", camera.enableMouseLook ? "ENABLED" : "DISABLED");

        ImGui::End();
    }

    bool DebugSystem::IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos) {
        float worldX = static_cast<float>(chunkPos.x * Game::Math::CHUNK_SIZE_X);
        float worldZ = static_cast<float>(chunkPos.z * Game::Math::CHUNK_SIZE_Z);

        AABB chunkAABB;
        chunkAABB.min = glm::vec3(worldX, Config::MinY, worldZ);
        chunkAABB.max = glm::vec3(
            worldX + Game::Math::CHUNK_SIZE_X,
            Config::MaxY,
            worldZ + Game::Math::CHUNK_SIZE_Z
        );

        return frustum.IsBoxVisible(chunkAABB);
    }

    void DebugSystem::DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum) {
        ImGui::Begin("Chunk Visualization");

        int cameraChunkX = static_cast<int>(std::floor(camera.position.x / Game::Math::CHUNK_SIZE_X));
        int cameraChunkZ = static_cast<int>(std::floor(camera.position.z / Game::Math::CHUNK_SIZE_Z));

        const int vizRadius = 12;
        const float circleRadius = 8.0f;
        const float spacing = 20.0f;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();

        if (canvasSize.x < 500) canvasSize.x = 500;
        if (canvasSize.y < 500) canvasSize.y = 500;

        ImVec2 center = ImVec2(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);

        // Draw grid
        ImU32 gridColor = IM_COL32(64, 64, 64, 128);
        for (int i = -vizRadius; i <= vizRadius; i++) {
            float x = center.x + i * spacing;
            drawList->AddLine(
                ImVec2(x, canvasPos.y),
                ImVec2(x, canvasPos.y + canvasSize.y),
                gridColor, 1.0f
            );

            float y = center.y + i * spacing;
            drawList->AddLine(
                ImVec2(canvasPos.x, y),
                ImVec2(canvasPos.x + canvasSize.x, y),
                gridColor, 1.0f
            );
        }

        int totalChunks = 0;
        int generatedChunks = 0;
        int visibleChunks = 0;

        // Draw chunks
        for (int dz = -vizRadius; dz <= vizRadius; dz++) {
            for (int dx = -vizRadius; dx <= vizRadius; dx++) {
                totalChunks++;

                Game::Math::ChunkPos chunkPos = {cameraChunkX + dx, cameraChunkZ + dz};

                // Check if chunk is loaded
                bool isGenerated = false;
                if (Game::g_world) {
                    isGenerated = Game::g_world->IsChunkLoaded(chunkPos.x, chunkPos.z);
                }

                bool inFrustum = false;
                if (isGenerated) {
                    generatedChunks++;
                    inFrustum = IsChunkInFrustum(frustum, chunkPos);
                    if (inFrustum) {
                        visibleChunks++;
                    }
                }

                ImU32 chunkColor;
                if (!isGenerated) {
                    chunkColor = IM_COL32(255, 64, 64, 255);
                } else if (inFrustum) {
                    chunkColor = IM_COL32(64, 255, 64, 255);
                } else {
                    chunkColor = IM_COL32(255, 255, 64, 255);
                }

                ImVec2 chunkScreenPos = ImVec2(
                    center.x + dx * spacing,
                    center.y + dz * spacing
                );

                drawList->AddCircleFilled(chunkScreenPos, circleRadius, chunkColor);

                ImU32 outlineColor = IM_COL32(128, 128, 128, 255);
                drawList->AddCircle(chunkScreenPos, circleRadius, outlineColor, 0, 2.0f);

                if (ImGui::IsMouseHoveringRect(
                    ImVec2(chunkScreenPos.x - circleRadius, chunkScreenPos.y - circleRadius),
                    ImVec2(chunkScreenPos.x + circleRadius, chunkScreenPos.y + circleRadius))) {

                    ImGui::SetTooltip("Chunk (%d, %d)\nGenerated: %s\nIn Frustum: %s",
                                     chunkPos.x, chunkPos.z,
                                     isGenerated ? "Yes" : "No",
                                     inFrustum ? "Yes" : "No");
                }
            }
        }

        // Draw player position
        ImVec2 playerPos = center;
        drawList->AddCircleFilled(playerPos, circleRadius + 2.0f, IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(playerPos, circleRadius + 2.0f, IM_COL32(0, 0, 0, 255), 0, 3.0f);

        // Draw player direction
        float playerYawRad = glm::radians(camera.yaw);
        ImVec2 directionEnd = ImVec2(
            playerPos.x + cos(playerYawRad) * (circleRadius + 8.0f),
            playerPos.y + sin(playerYawRad) * (circleRadius + 8.0f)
        );
        drawList->AddLine(playerPos, directionEnd, IM_COL32(0, 0, 0, 255), 3.0f);

        ImGui::InvisibleButton("chunk_viz_canvas", canvasSize);

        ImGui::Separator();
        ImGui::Text("Chunk Statistics:");
        ImGui::Text("Total chunks in view: %d", totalChunks);
        ImGui::Text("Generated chunks: %d", generatedChunks);
        ImGui::Text("Visible chunks: %d", visibleChunks);
        ImGui::Text("Camera chunk: (%d, %d)", cameraChunkX, cameraChunkZ);

        ImGui::Separator();
        ImGui::Text("Legend:");
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "● Red: Not generated");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.25f, 1.0f), "● Yellow: Generated, not visible");
        ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.25f, 1.0f), "● Green: Generated and visible");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "● White: Player position");

        ImGui::End();
    }

    void DebugSystem::DrawTextureAtlasDebug() {
        if (!ImGui::Begin("Texture Atlas Debug")) {
            ImGui::End();
            return;
        }

        // Check if we have the AtlasBuilder system
        if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
            ImGui::Text("=== ATLAS BUILDER SYSTEM ===");
            DrawAtlasBuilderDebug();
        } else {
            ImGui::Text("=== ATLAS BUILDER SYSTEM NOT AVAILABLE ===");
            ImGui::Text("The texture atlas system is not initialized.");
        }

        ImGui::End();
    }

    void DebugSystem::DrawAtlasBuilderDebug() {
        auto& atlasBuilder = *Render::g_atlasBuilder;

        ImGui::Text("Atlas Builder Status: Active");
        ImGui::Text("Atlas Size: %dx%d pixels", atlasBuilder.GetAtlasWidth(), atlasBuilder.GetAtlasHeight());
        ImGui::Text("Total Textures Loaded: %zu", atlasBuilder.GetTextureCount());

        // Texture IDs
        GLuint atlasID = atlasBuilder.GetAtlasTextureID();
        ImGui::Text("Main Atlas Texture ID: %u", atlasID);

        ImGui::Separator();

        // Atlas preview with zoom controls
        ImGui::Text("Atlas Preview (Scroll to navigate):");

        static float zoomLevel = 1.0f;
        ImGui::SliderFloat("Zoom", &zoomLevel, 0.25f, 10.0f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::Button("Reset Zoom")) {
            zoomLevel = 1.0f;
        }

        // Calculate display size based on atlas dimensions and zoom
        float aspectRatio = static_cast<float>(atlasBuilder.GetAtlasHeight()) / static_cast<float>(atlasBuilder.GetAtlasWidth());
        float baseDisplayWidth = 400.0f;
        float baseDisplayHeight = baseDisplayWidth * aspectRatio;
        float displayWidth = baseDisplayWidth * zoomLevel;
        float displayHeight = baseDisplayHeight * zoomLevel;

        ImVec2 scrollRegionSize = ImVec2(450, 500);
        ImGui::BeginChild("AtlasBuilderScrollRegion", scrollRegionSize, true, ImGuiWindowFlags_HorizontalScrollbar);

        ImTextureID textureID = (ImTextureID)(uintptr_t)atlasID;
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();

        // Display the atlas texture
        ImGui::Image(textureID,
                    ImVec2(displayWidth, displayHeight),
                    ImVec2(0, 0), ImVec2(1, 1));

        // Mouse interaction for texture coordinates
        if (ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
            ImVec2 mousePos = ImGui::GetMousePos();
            if (mousePos.x >= cursorPos.x && mousePos.x <= cursorPos.x + displayWidth &&
                mousePos.y >= cursorPos.y && mousePos.y <= cursorPos.y + displayHeight) {

                // Calculate UV coordinates
                float relativeX = (mousePos.x - cursorPos.x) / displayWidth;
                float relativeY = (mousePos.y - cursorPos.y) / displayHeight;

                // Clamp to valid range
                relativeX = std::max(0.0f, std::min(1.0f, relativeX));
                relativeY = std::max(0.0f, std::min(1.0f, relativeY));

                // Calculate pixel coordinates
                int pixelX = static_cast<int>(relativeX * atlasBuilder.GetAtlasWidth());
                int pixelY = static_cast<int>(relativeY * atlasBuilder.GetAtlasHeight());

                // Show tooltip with coordinates
                ImGui::SetTooltip("UV: (%.3f, %.3f)\nPixel: (%d, %d)\nAtlas Size: %dx%d",
                                 relativeX, relativeY, pixelX, pixelY,
                                 atlasBuilder.GetAtlasWidth(), atlasBuilder.GetAtlasHeight());

                // Draw crosshair at mouse position
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImU32 crosshairColor = IM_COL32(255, 0, 0, 255);

                // Vertical line
                drawList->AddLine(
                    ImVec2(mousePos.x, cursorPos.y),
                    ImVec2(mousePos.x, cursorPos.y + displayHeight),
                    crosshairColor, 1.0f
                );

                // Horizontal line
                drawList->AddLine(
                    ImVec2(cursorPos.x, mousePos.y),
                    ImVec2(cursorPos.x + displayWidth, mousePos.y),
                    crosshairColor, 1.0f
                );
            }
        }

        ImGui::EndChild();
    }

    // Simplified world debug
    void DebugSystem::DrawWorldDebug() {
        if (!ImGui::Begin("World Debug")) {
            ImGui::End();
            return;
        }

        ImGui::Text("=== WORLD STATUS ===");
        ImGui::Separator();

        if (Game::g_world) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ World System Active");

            // Get world path if available
            const std::string& worldPath = Game::g_world->GetMinecraftWorldPath();
            if (!worldPath.empty()) {
                ImGui::Text("World Path: %s", worldPath.c_str());
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Minecraft World Loaded");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⚠ Using Procedural Generation");
            }

            ImGui::Spacing();
            ImGui::Text("Loaded Chunks: %zu", Game::g_world->GetLoadedChunkCount());
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "✗ World System Not Available");
        }

        // World controls
        ImGui::Separator();
        ImGui::Text("=== WORLD CONTROLS ===");

        static char worldPathBuffer[512] = "";
        ImGui::InputText("World Path", worldPathBuffer, sizeof(worldPathBuffer));

        if (ImGui::Button("Load World") && Game::g_world) {
            std::string path = worldPathBuffer;
            if (!path.empty()) {
                Game::g_world->SetMinecraftWorldPath(path);
                Log::Info("Set world path: %s", path.c_str());
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear World") && Game::g_world) {
            Game::g_world->SetMinecraftWorldPath("");
            Log::Info("Cleared world path, using procedural generation");
        }

        // Quick load buttons for common locations
        ImGui::Spacing();
        ImGui::Text("Quick Load:");

        if (ImGui::Button("./world") && Game::g_world) {
            Game::g_world->SetMinecraftWorldPath("./world");
            strncpy(worldPathBuffer, "./world", sizeof(worldPathBuffer) - 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("../world") && Game::g_world) {
            Game::g_world->SetMinecraftWorldPath("../world");
            strncpy(worldPathBuffer, "../world", sizeof(worldPathBuffer) - 1);
        }

        ImGui::End();
    }

    void DebugSystem::DrawPlayerFlightControls(Game::PlayerController& playerController) {
        if (!ImGui::Begin("Player Flight Speed")) {
            ImGui::End();
            return;
        }

        ImGui::Text("=== NOCLIP FLIGHT CONTROLS ===");
        ImGui::Separator();

        // Get a non-const reference to physics
        auto& physics = playerController.GetPhysics();
        
        // Display current state
        if (physics.noclip) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Noclip Mode Active");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⚠ Noclip Mode Disabled");
            ImGui::Text("Press 'N' to enable noclip mode");
        }
        
        ImGui::Spacing();
        
        // Horizontal speed slider
        ImGui::Text("Horizontal Speed (WASD):");
        if (ImGui::SliderFloat("##HorizontalSpeed", &physics.noclipHorizontalSpeed, 1.0f, 50.0f, "%.1f blocks/s")) {
            // Speed updated directly through the reference
        }
        
        // Vertical speed slider  
        ImGui::Text("Vertical Speed (Space/Shift):");
        if (ImGui::SliderFloat("##VerticalSpeed", &physics.noclipVerticalSpeed, 1.0f, 50.0f, "%.1f blocks/s")) {
            // Speed updated directly through the reference
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        
        // Reset buttons
        if (ImGui::Button("Reset to Defaults")) {
            physics.noclipHorizontalSpeed = Game::PlayerPhysics::NOCLIP_HORIZONTAL_SPEED;
            physics.noclipVerticalSpeed = Game::PlayerPhysics::NOCLIP_VERTICAL_SPEED;
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Fast Preset")) {
            physics.noclipHorizontalSpeed = 25.0f;
            physics.noclipVerticalSpeed = 25.0f;
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Slow Preset")) {
            physics.noclipHorizontalSpeed = 5.0f;
            physics.noclipVerticalSpeed = 5.0f;
        }
        
        // Display current values
        ImGui::Spacing();
        ImGui::Text("Current Settings:");
        ImGui::Text("  Horizontal: %.1f blocks/s", physics.noclipHorizontalSpeed);
        ImGui::Text("  Vertical: %.1f blocks/s", physics.noclipVerticalSpeed);
        
        // Instructions
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Controls:");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "  N - Toggle noclip mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "  WASD - Horizontal movement");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "  Space - Fly up");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "  Shift - Fly down");
        
        ImGui::End();
    }
    
    void DebugSystem::ApplyRenderingMode(bool useMinecraftStyle) {
        if (!Render::g_atlasBuilder) {
            Log::Warning("Cannot apply rendering mode: AtlasBuilder not available");
            return;
        }
        
        GLuint atlasID = Render::g_atlasBuilder->GetAtlasTextureID();
        if (atlasID == 0) {
            Log::Warning("Cannot apply rendering mode: Atlas texture not created");
            return;
        }
        
        // Rebuild the atlas with the appropriate settings
        Render::g_atlasBuilder->RebuildAtlas(useMinecraftStyle);
    }

#else // NDEBUG - Release builds
    bool DebugSystem::Initialize(GLFWwindow* window) {
        return true; // No-op in release
    }

    void DebugSystem::Shutdown() {
        // No-op in release
    }

    void DebugSystem::BeginFrame() {
        // No-op in release
    }

    void DebugSystem::EndFrame() {
        // No-op in release
    }

    void DebugSystem::RenderDebugUI(
        const Render::Camera& camera,
        const Frustum& frustum,
        Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {
        // No-op in release
    }
#endif // NDEBUG

} // namespace Debug