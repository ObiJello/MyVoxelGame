// File: src/render/debug/DebugSystem.cpp (ENHANCED - Added Mesh System Debug)
#include "DebugSystem.hpp"
#include "../core/Log.hpp"
#include "../engine/block/BlockRegistry.hpp"
#include "Crosshair.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../core/Config.hpp"
#include <unordered_set>
#include <cmath>

#include "engine/world/ChunkProvider.hpp"
#include "engine/world/RegionFileCache.hpp"

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
        const Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {

        DrawMainDebugWindow(camera, playerController, metrics, cursorEnabled,
                           windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        DrawChunkVisualization(camera, frustum);
        DrawTextureAtlasDebug();
        DrawMinecraftWorldDebug();
        DrawMeshSystemDebug(); // **NEW**: Add mesh system debug window
    }

    void DebugSystem::DrawMainDebugWindow(
        const Render::Camera& camera,
        const Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {

        ImGui::Begin("Voxel Engine Debug");

        // Performance metrics
        ImGui::Text("Performance");
        ImGui::Separator();
        ImGui::Text("FPS: %.1f (%.2f ms)", metrics.GetFPS(), metrics.GetAverageFrameTime() * 1000.0f);
        ImGui::Text("Frame Time: %.2f ms", metrics.frameTime);
        ImGui::Text("Mesh Upload: %.2f ms", metrics.meshUploadTime);
        ImGui::Text("Render Time: %.2f ms", metrics.renderTime);
        ImGui::Spacing();

        // **NEW**: Enhanced mesh system metrics
        ImGui::Text("Enhanced Mesh System");
        ImGui::Separator();
        ImGui::Text("Meshes Built This Frame: %d", metrics.meshesUploadedThisFrame);
        ImGui::Text("Meshes Rendered This Frame: %d", metrics.meshesRenderedThisFrame);
        ImGui::Text("Total Vertices Rendered: %zu", metrics.totalVerticesRendered);
        ImGui::Text("Total Indices Rendered: %zu", metrics.totalIndicesRendered);

        // Memory usage estimate
        float memoryMB = (metrics.totalVerticesRendered * sizeof(Render::Vertex) +
                         metrics.totalIndicesRendered * sizeof(uint32_t)) / (1024.0f * 1024.0f);
        ImGui::Text("Estimated GPU Memory: %.2f MB", memoryMB);
        ImGui::Spacing();

        // Display Information
        ImGui::Text("Display Information");
        ImGui::Separator();
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

        // **NEW**: Mipmap controls
        static bool mipmapEnabled = false; // Default to enabled
        bool mipmapChanged = ImGui::Checkbox("Enable Mipmaps", &mipmapEnabled);

        if (mipmapChanged) {
            // Apply mipmap setting to AtlasBuilder
            if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
                Render::g_atlasBuilder->SetMipmapEnabled(mipmapEnabled);
                Log::Info("AtlasBuilder mipmaps %s", mipmapEnabled ? "enabled" : "disabled");
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mipmaps improve texture quality at distance\nbut may cause slight blurring.\n\nEnabled: GL_NEAREST_MIPMAP_LINEAR\nDisabled: GL_NEAREST");
        }

        ImGui::Spacing();

        // World statistics
        ImGui::Text("World Statistics");
        ImGui::Separator();
        glm::vec3 camPos = camera.position;
        ImGui::Text("Camera Position: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
        ImGui::Text("Camera Rotation: Yaw=%.1f°, Pitch=%.1f°", camera.yaw, camera.pitch);

        // Calculate current chunk
        int cameraChunkX = static_cast<int>(std::floor(camPos.x / Game::Math::CHUNK_SIZE_X));
        int cameraChunkZ = static_cast<int>(std::floor(camPos.z / Game::Math::CHUNK_SIZE_Z));
        ImGui::Text("Current Chunk: (%d, %d)", cameraChunkX, cameraChunkZ);

        size_t loadedChunks = Game::ChunkProvider::GetLoadedChunkCount();
        ImGui::Text("Loaded Chunks: %zu", loadedChunks);
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

    // **NEW**: Dedicated mesh system debug window
    void DebugSystem::DrawMeshSystemDebug() {
        if (!ImGui::Begin("Enhanced Mesh System")) {
            ImGui::End();
            return;
        }

        ImGui::Text("=== ENHANCED MESH SYSTEM STATUS ===");
        ImGui::Separator();

        // Note: In a real implementation, you'd get these stats from the mesh manager
        // For now, we'll show placeholder information

        ImGui::Text("System Status: Active");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Three-Layer Rendering");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Fluid Mesh Builder");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Frustum Culling");
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Distance-Based Sorting");

        ImGui::Spacing();
        ImGui::Text("Render Layers");
        ImGui::Separator();

        // Render layer information
        ImGui::Text("Opaque Layer:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Solid blocks (stone, dirt, etc.)");

        ImGui::Text("Cutout Layer:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Alpha-test blocks (leaves, grass)");

        ImGui::Text("Translucent Layer:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Blended blocks (glass, water, ice)");

        ImGui::Spacing();
        ImGui::Text("Fluid System");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "Water Rendering: Enhanced");
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), "Lava Rendering: Enhanced");
        ImGui::Text("Features:");
        ImGui::BulletText("Sloped fluid surfaces");
        ImGui::BulletText("Proper transparency");
        ImGui::BulletText("Flow detection");
        ImGui::BulletText("Side quad culling");

        ImGui::Spacing();
        ImGui::Text("Performance Features");
        ImGui::Separator();
        ImGui::BulletText("Face culling optimization");
        ImGui::BulletText("Frustum culling");
        ImGui::BulletText("Layer-based rendering");
        ImGui::BulletText("Distance-based mesh cleanup");
        ImGui::BulletText("Threaded mesh generation");
        ImGui::BulletText("Frame-time limited building");

        ImGui::Spacing();
        ImGui::Text("Mesh Generation Settings");
        ImGui::Separator();

        static int maxMeshesPerFrame = 2;
        if (ImGui::SliderInt("Max Meshes Per Frame", &maxMeshesPerFrame, 1, 10)) {
            // In real implementation, you'd update the mesh manager config
            Log::Debug("Max meshes per frame set to: %d", maxMeshesPerFrame);
        }

        static float maxBuildTimeMs = 5.0f;
        if (ImGui::SliderFloat("Max Build Time (ms)", &maxBuildTimeMs, 1.0f, 20.0f, "%.1f")) {
            // In real implementation, you'd update the mesh manager config
            Log::Debug("Max build time set to: %.1f ms", maxBuildTimeMs);
        }

        static int meshRadius = 8;
        if (ImGui::SliderInt("Mesh Radius", &meshRadius, 4, 16)) {
            // In real implementation, you'd update the mesh manager config
            Log::Debug("Mesh radius set to: %d chunks", meshRadius);
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Technical Details")) {
            ImGui::Text("Vertex Format:");
            ImGui::BulletText("Position: vec3 (world coordinates)");
            ImGui::BulletText("Normal: vec3 (face normal)");
            ImGui::BulletText("UV: vec2 (atlas coordinates)");
            ImGui::BulletText("Color: vec4 (biome tinting)");
            ImGui::BulletText("AO: uint8 (ambient occlusion)");

            ImGui::Spacing();
            ImGui::Text("Memory Layout:");
            ImGui::BulletText("Vertex size: %zu bytes", sizeof(Render::Vertex));
            ImGui::BulletText("Index size: 4 bytes (uint32_t)");
            ImGui::BulletText("Separate VBOs per layer");
            ImGui::BulletText("Indexed rendering");
        }

        ImGui::End();
    }

    // [Keep all other existing methods unchanged]
    bool DebugSystem::IsChunkInFrustum(const Frustum& frustum, Game::Math::ChunkPos chunkPos) {
        float worldX = static_cast<float>(chunkPos.x * Game::Math::CHUNK_SIZE_X);
        float worldZ = static_cast<float>(chunkPos.z * Game::Math::CHUNK_SIZE_Z);

        AABB chunkAABB;
        chunkAABB.min = glm::vec3(worldX, 0.0f, worldZ);
        chunkAABB.max = glm::vec3(
            worldX + Game::Math::CHUNK_SIZE_X,
            Game::Math::CHUNK_TOTAL_HEIGHT,
            worldZ + Game::Math::CHUNK_SIZE_Z
        );

        return frustum.IsBoxVisible(chunkAABB);
    }

    void DebugSystem::DrawChunkVisualization(const Render::Camera& camera, const Frustum& frustum) {
        ImGui::Begin("Chunk Visualization");

        int cameraChunkX = static_cast<int>(std::floor(camera.position.x / Game::Math::CHUNK_SIZE_X));
        int cameraChunkZ = static_cast<int>(std::floor(camera.position.z / Game::Math::CHUNK_SIZE_Z));

        std::unordered_set<uint64_t> loadedChunkSet;

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
                bool isGenerated = Game::ChunkProvider::IsChunkLoaded(chunkPos);

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
            ImGui::Text("=== ATLAS BUILDER SYSTEM FAILED ===");
        }

        ImGui::End();
    }

    void DebugSystem::DrawAtlasBuilderDebug() {
        auto& atlasBuilder = *Render::g_atlasBuilder;

        ImGui::Text("Atlas Builder Status: Active");
        ImGui::Text("Atlas Size: %dx%d pixels", atlasBuilder.GetAtlasWidth(), atlasBuilder.GetAtlasHeight());
        ImGui::Text("Total Textures Loaded: %zu", atlasBuilder.GetTextureCount());
        ImGui::Text("Successfully Packed: %zu", atlasBuilder.GetPackedCount());

        // Texture IDs
        GLuint atlasID = atlasBuilder.GetAtlasTextureID();
        GLuint grassID = atlasBuilder.GetGrassColormapID();
        GLuint foliageID = atlasBuilder.GetFoliageColormapID();

        ImGui::Text("Main Atlas Texture ID: %u", atlasID);
        ImGui::Text("Grass Colormap ID: %u", grassID);
        ImGui::Text("Foliage Colormap ID: %u", foliageID);

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

        // Colormap previews (if available)
        if (grassID != 0 || foliageID != 0) {
            ImGui::Separator();
            ImGui::Text("Biome Colormaps:");

            if (grassID != 0) {
                ImGui::Text("Grass Colormap:");
                ImTextureID grassTextureID = (ImTextureID)(uintptr_t)grassID;
                ImGui::Image(grassTextureID, ImVec2(128, 128));
                ImGui::SameLine();
            }

            if (foliageID != 0) {
                ImGui::Text("Foliage Colormap:");
                ImTextureID foliageTextureID = (ImTextureID)(uintptr_t)foliageID;
                ImGui::Image(foliageTextureID, ImVec2(128, 128));
            }
        }

        // Debug save button
        ImGui::Separator();
        if (ImGui::Button("Save Atlas Debug Image")) {
            if (atlasBuilder.SaveAtlasDebugImage("debug_atlas_output.png")) {
                Log::Info("Saved atlas debug image to debug_atlas_output.png");
            } else {
                Log::Error("Failed to save atlas debug image");
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Saves the current atlas texture as a PNG file\nfor debugging texture placement and quality");
        }
    }

    // Debug UI for Minecraft world support
    void DebugSystem::DrawMinecraftWorldDebug() {
        if (!ImGui::Begin("Minecraft World Support")) {
            ImGui::End();
            return;
        }

        auto stats = Game::ChunkProvider::GetWorldStats();

        ImGui::Text("=== MINECRAFT WORLD STATUS ===");
        ImGui::Separator();

        if (stats.hasMinecraftWorld) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "✓ Minecraft World Loaded");
            ImGui::Text("World Path: %s", stats.worldPath.c_str());

            // Check if region directory exists
            std::string regionPath = stats.worldPath + "/region";
            if (std::filesystem::exists(regionPath)) {
                // Count region files
                int regionCount = 0;
                try {
                    for (const auto& entry : std::filesystem::directory_iterator(regionPath)) {
                        if (entry.path().extension() == ".mca") {
                            regionCount++;
                        }
                    }
                } catch (...) {
                    regionCount = -1;
                }

                if (regionCount >= 0) {
                    ImGui::Text("Region Files: %d", regionCount);
                } else {
                    ImGui::Text("Region Files: Error counting");
                }
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⚠ No Minecraft World");
            ImGui::Text("Using procedural generation");
        }

        ImGui::Spacing();
        ImGui::Text("Loaded Chunks: %zu", stats.loadedChunks);

        // World loading interface
        ImGui::Separator();
        ImGui::Text("=== LOAD MINECRAFT WORLD ===");

        static char worldPathBuffer[512] = "";
        ImGui::InputText("World Path", worldPathBuffer, sizeof(worldPathBuffer));

        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            // In a real implementation, you'd open a file dialog here
            ImGui::OpenPopup("World Browser");
        }

        if (ImGui::Button("Load World")) {
            std::string path = worldPathBuffer;
            if (!path.empty()) {
                if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                    Log::Info("Successfully loaded world from UI: %s", path.c_str());
                } else {
                    Log::Error("Failed to load world from UI: %s", path.c_str());
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear World")) {
            Game::ChunkProvider::ClearAllChunks();
            Game::ChunkProvider::SetMinecraftWorldPath("");
            Log::Info("Cleared world and switched to procedural generation");
        }

        // Quick load buttons for common locations
        ImGui::Spacing();
        ImGui::Text("Quick Load:");

        std::vector<std::pair<std::string, std::string>> quickPaths = {
            {"Current Dir", "./world"},
            {"Parent Dir", "../world"},
            {"Test World", "./saves/TestWorld"}
        };

        for (const auto& [name, path] : quickPaths) {
            if (ImGui::Button(name.c_str())) {
                if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                    strncpy(worldPathBuffer, path.c_str(), sizeof(worldPathBuffer) - 1);
                    worldPathBuffer[sizeof(worldPathBuffer) - 1] = '\0';
                }
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        // Chunk loading statistics
        ImGui::Separator();
        ImGui::Text("=== CHUNK STATISTICS ===");

        // Test specific chunk loading
        static int testChunkX = 0, testChunkZ = 0;
        ImGui::InputInt("Test Chunk X", &testChunkX);
        ImGui::InputInt("Test Chunk Z", &testChunkZ);

        if (ImGui::Button("Test Chunk Availability")) {
            Game::Math::ChunkPos testPos{testChunkX, testChunkZ};
            bool available = Game::ChunkProvider::IsMinecraftChunkAvailable(testPos);

            if (available) {
                Log::Info("Chunk (%d, %d) is available in Minecraft world", testChunkX, testChunkZ);
            } else {
                Log::Info("Chunk (%d, %d) not found in Minecraft world (will be generated)", testChunkX, testChunkZ);
            }
        }

        // Region file cache statistics
        ImGui::Spacing();
        size_t cacheSize = World::RegionFileCache::Instance().GetCacheSize();
        ImGui::Text("Region File Cache: %zu files", cacheSize);

        if (ImGui::Button("Clear Region Cache")) {
            World::RegionFileCache::Instance().Clear();
            Log::Info("Cleared region file cache");
        }

        ImGui::End();
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
        const Game::PlayerController& playerController,
        const PerformanceMetrics& metrics,
        bool cursorEnabled,
        int windowWidth, int windowHeight,
        int framebufferWidth, int framebufferHeight) {
        // No-op in release
    }

    void DebugSystem::DrawMeshSystemDebug() {
        // No-op in release
    }
#endif // NDEBUG

} // namespace Debug