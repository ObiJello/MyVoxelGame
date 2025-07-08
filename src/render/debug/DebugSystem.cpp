// File: src/render/debug/DebugSystem.cpp
#include "DebugSystem.hpp"
#include "../core/Log.hpp"
#include "../engine/world/WorldManager.hpp"
#include "../engine/block/BlockRegistry.hpp"
#include "../mesh/ChunkRenderer.hpp"
#include "Crosshair.hpp"
#include "../atlas/AtlasBuilder.hpp"
#include "../core/Config.hpp"
#include <unordered_set>
#include <cmath>


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
        ImGui::Text("Loaded Chunks: %zu", Game::WorldManager::GetLoadedChunkCount());
        ImGui::Text("Rendered Sections: %d / %zu", metrics.meshesRenderedThisFrame, Render::g_chunkMeshes.size());
        ImGui::Text("Meshes Uploaded This Frame: %d", metrics.meshesUploadedThisFrame);
        ImGui::Spacing();

        // Rendering statistics
        ImGui::Text("Rendering Statistics");
        ImGui::Separator();
        ImGui::Text("Total Vertices Rendered: %zu", metrics.totalVerticesRendered);
        ImGui::Text("Total Indices Rendered: %zu", metrics.totalIndicesRendered);
        ImGui::Text("Frustum Culled Sections: %zu",
                   Render::g_chunkMeshes.size() - metrics.meshesRenderedThisFrame);

        // Physics information
        ImGui::Spacing();
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

        // Force remesh button for testing
        if (ImGui::Button("Force Remesh Current Chunk")) {
            int chunkX = static_cast<int>(std::floor(camPos.x / Game::Math::CHUNK_SIZE_X));
            int chunkZ = static_cast<int>(std::floor(camPos.z / Game::Math::CHUNK_SIZE_Z));
            Game::WorldManager::ForceRemeshChunk({chunkX, chunkZ});
            Log::Info("Force remesh requested for chunk (%d, %d)", chunkX, chunkZ);
        }

        ImGui::End();
    }

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

        auto loadedChunks = Game::WorldManager::GetLoadedChunks();
        std::unordered_set<uint64_t> loadedChunkSet;

        for (const auto& chunk : loadedChunks) {
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunk.x)) << 32) |
                          static_cast<uint32_t>(chunk.z);
            loadedChunkSet.insert(key);
        }

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

                uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.x)) << 32) |
                              static_cast<uint32_t>(chunkPos.z);
                bool isGenerated = loadedChunkSet.find(key) != loadedChunkSet.end();

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

    // Replace the existing DrawTextureAtlasDebug method with this updated version:
    void DebugSystem::DrawTextureAtlasDebug() {
        if (!ImGui::Begin("Texture Atlas Debug")) {
            // Window is collapsed or not visible, skip all content
            ImGui::End();
            return;
        }

        // Check if we have the new AtlasBuilder system
        if (Render::g_atlasBuilder && Render::g_atlasBuilder->GetAtlasTextureID() != 0) {
            ImGui::Text("=== ATLAS BUILDER SYSTEM ===");
            DrawAtlasBuilderDebug();
        } else {
            ImGui::Text("=== ATLAS BUILDER SYSTEM FAILED===");
        }

        ImGui::End();
    }

    // Add this new method to show AtlasBuilder debug info
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
#endif // NDEBUG

} // namespace Debug