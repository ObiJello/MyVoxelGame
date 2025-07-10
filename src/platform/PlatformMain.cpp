// File: src/platform/PlatformMain.cpp (UPDATED - Integrated New Mesher System)
#include "PlatformMain.hpp"
#include "Time.hpp"
#include "Input.hpp"
#include "Log.hpp"
#include "Config.hpp"
#include "../render/debug/DebugSystem.hpp"

// Include game headers
#include "../engine/block/BlockRegistry.hpp"
#include "../engine/block/BlockModel.hpp"
#include "../engine/world/ChunkProvider.hpp"
#include "../engine/world/World.hpp"
#include "../game/PlayerController.hpp"
#include "../engine/physics/RayCast.hpp"
#include "../engine/physics/Physics.hpp"

// Include rendering headers
#include "../render/gfx/Camera.hpp"
#include "../render/gfx/Frustum.hpp"
#include "../render/gfx/Shader.hpp"  // **FIX**: Make sure Shader is included
#include "../render/mesh/BlockHighlight.hpp"
#include "../render/debug/Crosshair.hpp"
#include "../render/atlas/AtlasBuilder.hpp"

// **NEW**: Include the new mesher system
#include "../render/mesh/MeshBuilder.hpp"
#include "../render/mesh/ChunkMeshData.hpp"
#include "../render/mesh/ChunkRenderer.hpp"
#include "../render/mesh/FluidMeshBuilder.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <queue>
#include <unordered_map>
#include <memory>

#include "JobSystem.hpp"
#include "engine/world/RegionFileCache.hpp"
#include "engine/world/SectionDataUnpacker.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>
#endif

namespace PlatformMain {

    // **NEW**: Chunk mesh management system
    struct ChunkMeshManager {
        // Global mesh storage - maps chunk positions to GPU mesh data
        std::unordered_map<uint64_t, std::unique_ptr<Render::ChunkMesh>> loadedMeshes;
        mutable std::mutex meshMutex;

        // Section-based mesh generation system
        std::unique_ptr<Render::MeshBuilder> meshBuilder;
        std::unique_ptr<Render::ChunkRenderer> chunkRenderer;

        // Performance tracking
        std::chrono::steady_clock::time_point lastMeshUpdate;
        int sectionsBuiltThisFrame = 0;
        int meshesBuiltThisFrame = 0;
        float totalSectionBuildTime = 0.0f;

        // Configuration
        static constexpr int MAX_SECTIONS_PER_FRAME = 8;    // Limit sections built per frame
        static constexpr int MAX_CHUNKS_PER_FRAME = 3;      // Limit chunks processed per frame
        static constexpr float MAX_SECTION_BUILD_TIME_MS = 8.0f; // Max time per frame for section building

        // Section building strategy
        enum class BuildStrategy {
            NEAREST_FIRST,     // Build sections closest to player first
            BOTTOM_UP,         // Build from bedrock upward
            VISIBLE_FIRST,     // Build visible sections first
            DIRTY_ONLY         // Only build sections marked as dirty
        };

        BuildStrategy currentStrategy = BuildStrategy::NEAREST_FIRST;

        // Convert chunk coordinates to key
        uint64_t MakeChunkKey(int chunkX, int chunkZ) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
                   static_cast<uint32_t>(chunkZ);
        }

        // Convert key back to coordinates
        std::pair<int, int> KeyToChunkCoords(uint64_t key) {
            int chunkX = static_cast<int32_t>(key >> 32);
            int chunkZ = static_cast<int32_t>(key & 0xFFFFFFFF);
            return {chunkX, chunkZ};
        }

        // Initialize the enhanced mesh system
        bool Initialize(Game::World& world, Render::AtlasBuilder& atlas, Shader& blockShader) {
            Log::Info("Initializing Enhanced Section-Based Mesh System...");

            // Create mesh builder with section support
            meshBuilder = std::make_unique<Render::MeshBuilder>(world, atlas);
            if (!meshBuilder) {
                Log::Error("Failed to create section-based MeshBuilder");
                return false;
            }

            // Configure mesh builder for optimal section building
            Render::MeshBuilder::SectionBuildConfig config;
            config.skipEmptySections = true;
            config.includeNeighborData = true;
            config.maxSectionsPerFrame = MAX_SECTIONS_PER_FRAME;
            config.enableFluidMeshing = true;
            meshBuilder->SetConfig(config);

            // Create chunk renderer
            chunkRenderer = std::make_unique<Render::ChunkRenderer>();
            if (!chunkRenderer->Initialize(blockShader, atlas)) {
                Log::Error("Failed to initialize ChunkRenderer");
                return false;
            }

            // Configure renderer for section-based rendering
            Render::ChunkRenderer::Config rendererConfig;
            rendererConfig.enableDepthTesting = true;
            rendererConfig.enableFaceCulling = true;
            rendererConfig.enableBlending = true;
            rendererConfig.sortTransparentMeshes = true;
            rendererConfig.maxRenderDistance = 256.0f;
            chunkRenderer->SetConfig(rendererConfig);

            lastMeshUpdate = std::chrono::steady_clock::now();

            Log::Info("✓ Enhanced Section-Based Mesh System initialized successfully");
            return true;
        }

        // Update mesh system - processes sections efficiently
        void Update(const Game::PlayerController& playerController) {
            auto now = std::chrono::steady_clock::now();
            float deltaTime = std::chrono::duration<float, std::milli>(now - lastMeshUpdate).count();
            lastMeshUpdate = now;

            // Reset frame counters
            sectionsBuiltThisFrame = 0;
            meshesBuiltThisFrame = 0;
            totalSectionBuildTime = 0.0f;

            // Get player chunk position
            glm::vec3 playerPos = playerController.GetPhysics().position;
            int playerChunkX = static_cast<int>(std::floor(playerPos.x / Game::Math::CHUNK_SIZE_X));
            int playerChunkZ = static_cast<int>(std::floor(playerPos.z / Game::Math::CHUNK_SIZE_Z));

            Log::Debug("Updating meshes around player chunk (%d, %d)", playerChunkX, playerChunkZ);

            // Process sections around player based on strategy
            ProcessSectionsAroundPlayer(playerChunkX, playerChunkZ, playerPos);

            // Clean up distant meshes
            CleanupDistantMeshes(playerChunkX, playerChunkZ, 12);
        }

        // Process sections around player using current strategy
        void ProcessSectionsAroundPlayer(int playerChunkX, int playerChunkZ, const glm::vec3& playerPos) {
            const int MESH_RADIUS = 8; // Process chunks within 8 chunks of player

            // Get candidate chunks sorted by distance
            std::vector<std::pair<float, std::pair<int, int>>> candidateChunks;

            for (int dx = -MESH_RADIUS; dx <= MESH_RADIUS; dx++) {
                for (int dz = -MESH_RADIUS; dz <= MESH_RADIUS; dz++) {
                    int chunkX = playerChunkX + dx;
                    int chunkZ = playerChunkZ + dz;

                    // Calculate distance to player
                    float chunkCenterX = chunkX * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f;
                    float chunkCenterZ = chunkZ * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f;
                    float distance = glm::length(glm::vec2(chunkCenterX - playerPos.x, chunkCenterZ - playerPos.z));

                    candidateChunks.push_back({distance, {chunkX, chunkZ}});
                }
            }

            // Sort by distance (nearest first)
            std::sort(candidateChunks.begin(), candidateChunks.end());

            // Process chunks up to frame limits
            int chunksProcessed = 0;
            for (const auto& [distance, coords] : candidateChunks) {
                if (chunksProcessed >= MAX_CHUNKS_PER_FRAME ||
                    sectionsBuiltThisFrame >= MAX_SECTIONS_PER_FRAME ||
                    totalSectionBuildTime >= MAX_SECTION_BUILD_TIME_MS) {
                    break;
                }

                auto [chunkX, chunkZ] = coords;
                if (ProcessChunkSections(chunkX, chunkZ, playerPos)) {
                    chunksProcessed++;
                }
            }

            if (sectionsBuiltThisFrame > 0) {
                Log::Debug("Built %d sections across %d chunks in %.2fms",
                          sectionsBuiltThisFrame, chunksProcessed, totalSectionBuildTime);
            }
        }

        // Process sections for a specific chunk
        bool ProcessChunkSections(int chunkX, int chunkZ, const glm::vec3& playerPos) {
            // Check if chunk is loaded
            auto chunk = Game::ChunkProvider::GetChunkIfLoaded({chunkX, chunkZ});
            if (!chunk) {
                return false;
            }

            // Get or create chunk mesh
            uint64_t key = MakeChunkKey(chunkX, chunkZ);
            Render::ChunkMesh* chunkMesh = GetOrCreateChunkMesh(key, chunkX, chunkZ);
            if (!chunkMesh) {
                return false;
            }

            // Get sections that need rebuilding
            auto dirtySections = chunk->GetDirtySections();
            if (dirtySections.empty()) {
                return false; // Nothing to do
            }

            Log::Debug("Processing %zu dirty sections for chunk (%d, %d)",
                      dirtySections.size(), chunkX, chunkZ);

            // Build sections based on current strategy
            auto sectionsToProcess = SelectSectionsToProcess(dirtySections, playerPos, chunkX, chunkZ);

            bool processedAnySections = false;
            for (int sectionIndex : sectionsToProcess) {
                if (sectionsBuiltThisFrame >= MAX_SECTIONS_PER_FRAME ||
                    totalSectionBuildTime >= MAX_SECTION_BUILD_TIME_MS) {
                    break;
                }

                if (BuildSection(chunkX, chunkZ, sectionIndex, chunkMesh)) {
                    chunk->ClearSectionDirty(sectionIndex);
                    chunk->MarkSectionMeshed(sectionIndex);
                    processedAnySections = true;
                }
            }

            return processedAnySections;
        }

        // Select which sections to process based on strategy
        std::vector<int> SelectSectionsToProcess(const std::unordered_set<int>& dirtySections,
                                               const glm::vec3& playerPos, int chunkX, int chunkZ) {
            std::vector<int> sectionsToProcess(dirtySections.begin(), dirtySections.end());

            switch (currentStrategy) {
                case BuildStrategy::NEAREST_FIRST: {
                    // Sort by distance to player
                    int playerSectionY = Game::Chunk::SectionIndexFromGlobalY(static_cast<int>(playerPos.y));
                    std::sort(sectionsToProcess.begin(), sectionsToProcess.end(),
                        [playerSectionY](int a, int b) {
                            return std::abs(a - playerSectionY) < std::abs(b - playerSectionY);
                        });
                    break;
                }

                case BuildStrategy::BOTTOM_UP:
                    // Sort from bottom to top
                    std::sort(sectionsToProcess.begin(), sectionsToProcess.end());
                    break;

                case BuildStrategy::VISIBLE_FIRST:
                    // TODO: Implement visibility-based sorting using frustum culling
                    // For now, fall back to nearest first
                    int playerSectionY = Game::Chunk::SectionIndexFromGlobalY(static_cast<int>(playerPos.y));
                    std::sort(sectionsToProcess.begin(), sectionsToProcess.end(),
                        [playerSectionY](int a, int b) {
                            return std::abs(a - playerSectionY) < std::abs(b - playerSectionY);
                        });
                    break;

                case BuildStrategy::DIRTY_ONLY:
                    // No sorting, process in arbitrary order
                    break;
            }

            return sectionsToProcess;
        }

        // Build a specific section
        bool BuildSection(int chunkX, int chunkZ, int sectionIndex, Render::ChunkMesh* chunkMesh) {
            auto startTime = std::chrono::high_resolution_clock::now();

            // Convert section index to section Y coordinate
            int sectionY = Config::MinY / Game::Math::SECTION_HEIGHT + sectionIndex;

            Log::Debug("Building section %d (Y=%d) for chunk (%d, %d)",
                      sectionIndex, sectionY, chunkX, chunkZ);

            try {
                // Build section mesh data
                Render::SectionMeshData sectionData = meshBuilder->BuildSection(chunkX, chunkZ, sectionY);

                // Upload to GPU if not empty
                if (!sectionData.IsEmpty()) {
                    chunkMesh->UploadSection(sectionIndex, sectionData);
                    Log::Debug("Uploaded section %d: %zu vertices, %zu indices",
                              sectionIndex, sectionData.GetTotalVertices(), sectionData.GetTotalIndices());
                } else {
                    Log::Debug("Section %d is empty, skipping upload", sectionIndex);
                }

                // Update statistics
                auto endTime = std::chrono::high_resolution_clock::now();
                float buildTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();

                sectionsBuiltThisFrame++;
                totalSectionBuildTime += buildTime;

                return true;

            } catch (const std::exception& e) {
                Log::Error("Exception building section %d for chunk (%d, %d): %s",
                          sectionIndex, chunkX, chunkZ, e.what());
                return false;
            }
        }

        // Get or create chunk mesh
        Render::ChunkMesh* GetOrCreateChunkMesh(uint64_t key, int chunkX, int chunkZ) {
            std::lock_guard<std::mutex> lock(meshMutex);

            auto it = loadedMeshes.find(key);
            if (it != loadedMeshes.end()) {
                return it->second.get();
            }

            // Create new chunk mesh
            auto chunkMesh = std::make_unique<Render::ChunkMesh>();
            chunkMesh->chunkX = chunkX;
            chunkMesh->chunkZ = chunkZ;

            Render::ChunkMesh* result = chunkMesh.get();
            loadedMeshes[key] = std::move(chunkMesh);

            Log::Debug("Created new chunk mesh for (%d, %d)", chunkX, chunkZ);
            return result;
        }

        // Render all visible chunks using section-based rendering
        void Render(const Render::Camera& camera, const Frustum& frustum, const glm::mat4& viewProj) {
            if (!chunkRenderer) {
                return;
            }

            std::lock_guard<std::mutex> lock(meshMutex);

            // Collect visible chunk meshes and their sections
            std::vector<Render::ChunkMesh*> visibleMeshes;
            std::vector<glm::vec3> chunkCenters;

            for (const auto& [key, chunkMesh] : loadedMeshes) {
                if (!chunkMesh || chunkMesh->IsEmpty()) continue;

                auto [chunkX, chunkZ] = KeyToChunkCoords(key);

                // Calculate chunk center
                glm::vec3 chunkCenter(
                    chunkX * Game::Math::CHUNK_SIZE_X + Game::Math::CHUNK_SIZE_X * 0.5f,
                    0.0f, // Will be adjusted by renderer
                    chunkZ * Game::Math::CHUNK_SIZE_Z + Game::Math::CHUNK_SIZE_Z * 0.5f
                );

                // Simple frustum culling on chunk AABB
                AABB chunkAABB;
                chunkAABB.min = glm::vec3(
                    chunkX * Game::Math::CHUNK_SIZE_X,
                    Config::MinY,
                    chunkZ * Game::Math::CHUNK_SIZE_Z
                );
                chunkAABB.max = glm::vec3(
                    (chunkX + 1) * Game::Math::CHUNK_SIZE_X,
                    Config::MaxY,
                    (chunkZ + 1) * Game::Math::CHUNK_SIZE_Z
                );

                if (frustum.IsBoxVisible(chunkAABB)) {
                    visibleMeshes.push_back(chunkMesh.get());
                    chunkCenters.push_back(chunkCenter);
                }
            }

            // Render all visible chunks with their sections
            chunkRenderer->RenderSectionBasedChunks(visibleMeshes, chunkCenters, camera, viewProj);
        }

        // Cleanup distant meshes
        void CleanupDistantMeshes(int playerChunkX, int playerChunkZ, int maxDistance) {
            std::lock_guard<std::mutex> lock(meshMutex);

            std::vector<uint64_t> toRemove;

            for (const auto& [key, chunkMesh] : loadedMeshes) {
                auto [chunkX, chunkZ] = KeyToChunkCoords(key);

                int dx = chunkX - playerChunkX;
                int dz = chunkZ - playerChunkZ;
                float distance = std::sqrt(dx * dx + dz * dz);

                if (distance > maxDistance) {
                    toRemove.push_back(key);
                }
            }

            for (uint64_t key : toRemove) {
                loadedMeshes.erase(key);
            }

            if (!toRemove.empty()) {
                Log::Debug("Cleaned up %zu distant chunk meshes", toRemove.size());
            }
        }

        // Get enhanced mesh statistics
        struct EnhancedMeshStats {
            size_t totalChunks = 0;
            size_t totalSections = 0;
            size_t activeSections = 0;
            size_t totalMemoryBytes = 0;
            int sectionsBuiltThisFrame = 0;
            float sectionBuildTimeMs = 0.0f;
            size_t opaqueSections = 0;
            size_t cutoutSections = 0;
            size_t translucentSections = 0;
        };

        EnhancedMeshStats GetStats() const {
            std::lock_guard<std::mutex> lock(meshMutex);

            EnhancedMeshStats stats;
            stats.totalChunks = loadedMeshes.size();
            stats.sectionsBuiltThisFrame = sectionsBuiltThisFrame;
            stats.sectionBuildTimeMs = totalSectionBuildTime;

            for (const auto& [key, chunkMesh] : loadedMeshes) {
                if (chunkMesh) {
                    auto chunkStats = chunkMesh->GetStats();
                    stats.activeSections += chunkStats.activeSections;
                    stats.totalMemoryBytes += chunkStats.totalMemoryBytes;

                    // Count sections by type
                    for (const auto& section : chunkMesh->sections) {
                        if (section && !section->IsEmpty()) {
                            if (!section->opaque.IsEmpty()) stats.opaqueSections++;
                            if (!section->cutout.IsEmpty()) stats.cutoutSections++;
                            if (!section->translucent.IsEmpty()) stats.translucentSections++;
                        }
                    }
                }
            }

            stats.totalSections = stats.totalChunks * Game::Math::SECTIONS_PER_CHUNK;
            return stats;
        }

        // Change building strategy
        void SetBuildStrategy(BuildStrategy strategy) {
            currentStrategy = strategy;
            Log::Info("Changed section build strategy to: %d", static_cast<int>(strategy));
        }
    };

    // Global mesh manager instance
    static std::unique_ptr<ChunkMeshManager> g_chunkMeshManager;

    // [Keep all existing platform-specific functions unchanged]
    std::string GetAssetPath(const std::string& relativePath) {
#ifdef __APPLE__
        // On macOS, check if we're running from a bundle
        CFBundleRef mainBundle = CFBundleGetMainBundle();
        if (mainBundle) {
            // Get the Resources directory from the bundle
            CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
            if (resourcesURL) {
                char path[PATH_MAX];
                if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, PATH_MAX)) {
                    CFRelease(resourcesURL);

                    std::string resourcesPath = std::string(path);
                    std::string fullPath = resourcesPath + "/" + relativePath;

                    // Check if the asset exists in the bundle Resources directory
                    if (std::filesystem::exists(fullPath)) {
                        Log::Info("Using bundle asset path: %s", fullPath.c_str());
                        return fullPath;
                    } else {
                        Log::Debug("Asset not found in bundle Resources: %s", fullPath.c_str());
                    }
                }
                CFRelease(resourcesURL);
            }
        }

        // Fall back to relative path from current directory
        Log::Debug("Falling back to relative asset path: %s", relativePath.c_str());
        return relativePath;
#else
        // On other platforms, use relative path directly
        return relativePath;
#endif
    }

    // **UPDATED**: Enhanced rendering function with new mesher
    void RenderWorld(const Game::PlayerController& playerController,
                    const Render::Camera& camera,
                    const glm::mat4& viewProj) {
        if (!g_chunkMeshManager) {
            return;
        }

        // Create frustum for culling
        Frustum frustum = Frustum::FromMatrix(viewProj);

        // Render all chunks using the new system
        g_chunkMeshManager->Render(camera, frustum, viewProj);
    }

    void RenderBlockHighlight(const Game::PlayerController& playerController, const glm::mat4& viewProj) {
        const auto& hit = playerController.GetCurrentHit();
        if (Render::BlockHighlight::IsValidHighlight(hit)) {
            Render::g_blockHighlight.Render(hit->blockPos, viewProj);
        }
    }

    void RenderCrosshair(GLFWwindow* window) {
        int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

        Render::g_crosshair.Render(windowWidth, windowHeight, framebufferWidth, framebufferHeight);
    }

    // [Keep all existing initialization functions unchanged]
    void InitializeGameSystems() {
        Log::Info("Initializing game systems...");

        // Initialize block registries
        Game::BlockRegistry::Init();

        // Use platform-specific asset path function
        std::string modelsPath = GetAssetPath("assets/models/block");

        // Load block models
        if (!Game::BlockModelRegistry::LoadModels(modelsPath)) {
            Log::Warning("Failed to load block models from %s, using default models", modelsPath.c_str());
        }

        Log::Info("✓ Game systems initialized");
    }

    Shader InitializeShaders() {
        // Use platform-specific asset paths
        std::string vertPath = GetAssetPath("shaders/block.vert");
        std::string fragPath = GetAssetPath("shaders/block.frag");

        // Try to use shaders if available
        if (std::filesystem::exists(vertPath) && std::filesystem::exists(fragPath)) {
            Log::Info("Using block shaders");
            return Shader(vertPath, fragPath);
        }

        // **FIX**: Return a valid shader even if files don't exist
        Log::Warning("Shader files not found, creating basic fallback shader");
        return Shader(vertPath, fragPath); // This will create a basic shader even if files don't exist
    }

    bool InitializeTextureSystem() {
        // Initialize AtlasBuilder
        Render::g_atlasBuilder = std::make_unique<Render::AtlasBuilder>();
        std::string atlasJsonPath = GetAssetPath("assets/atlases/blocks.json");
        std::string texturesPath = GetAssetPath("assets/textures");

        if (!Render::g_atlasBuilder->BuildFromJSON(atlasJsonPath, texturesPath)) {
            Log::Warning("AtlasBuilder failed to build from JSON at %s",
                        atlasJsonPath.c_str());
            Render::g_atlasBuilder.reset();
            return false;
        }
        Log::Info("AtlasBuilder initialized successfully: %dx%d atlas with %zu textures",
                 Render::g_atlasBuilder->GetAtlasWidth(),
                 Render::g_atlasBuilder->GetAtlasHeight(),
                 Render::g_atlasBuilder->GetTextureCount());
        return true;
    }

    // [Keep all existing input and utility functions unchanged]
    void HandlePlayerInput(Game::PlayerController& playerController, Render::Camera& camera) {
        // Movement input
        glm::vec3 movementInput = camera.CalculateMovementInput();
        playerController.SetMovementInput(movementInput);

        // Action inputs
        playerController.SetJumpPressed(camera.IsJumpPressed());
        playerController.SetSprintPressed(camera.IsSprintPressed());
        playerController.SetSneakPressed(camera.IsSneakPressed());

        // Block interaction
        if (Input::IsMouseButtonDown(Input::Key::LeftMouse)) {
            playerController.OnBreakPressed();
        } else {
            playerController.OnBreakReleased();
        }

        if (Input::IsMouseButtonDown(Input::Key::RightMouse)) {
            playerController.OnPlacePressed();
        } else {
            playerController.OnPlaceReleased();
        }

        // Inventory selection
        if (Input::IsKeyPressed(Input::Key::Alpha1)) playerController.SelectSlot(0);
        if (Input::IsKeyPressed(Input::Key::Alpha2)) playerController.SelectSlot(1);
        if (Input::IsKeyPressed(Input::Key::Alpha3)) playerController.SelectSlot(2);
        if (Input::IsKeyPressed(Input::Key::Alpha4)) playerController.SelectSlot(3);
        if (Input::IsKeyPressed(Input::Key::Alpha5)) playerController.SelectSlot(4);
        if (Input::IsKeyPressed(Input::Key::Alpha6)) playerController.SelectSlot(5);
        if (Input::IsKeyPressed(Input::Key::Alpha7)) playerController.SelectSlot(6);
        if (Input::IsKeyPressed(Input::Key::Alpha8)) playerController.SelectSlot(7);
        if (Input::IsKeyPressed(Input::Key::Alpha9)) playerController.SelectSlot(8);

        // Mouse wheel for inventory scrolling
        auto [scrollX, scrollY] = Input::GetScrollOffset();
        if (scrollY > 0) {
            playerController.SelectPreviousSlot();
        } else if (scrollY < 0) {
            playerController.SelectNextSlot();
        }

        // Debug noclip toggle
        if (Input::IsKeyPressed(Input::Key::N)) {
            playerController.ToggleNoclip();
        }
    }

    bool HandleCursorToggle(GLFWwindow* window, Render::Camera& camera) {
        static bool cursorEnabled = false;

        if (Input::IsKeyPressed(Input::Key::Tab)) {
            cursorEnabled = !cursorEnabled;

            if (cursorEnabled) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                camera.enableMouseLook = false;
                Log::Info("Cursor enabled - camera mouse look disabled");
            } else {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                camera.enableMouseLook = true;
                Log::Info("Cursor disabled - camera mouse look enabled");
            }
        }

        return cursorEnabled;
    }

    void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                const GLchar* message, const void* userParam) {
        Log::Error("[GL DEBUG] %s", message);
    }

    bool InitializeMinecraftSupport() {
        Log::Info("=== MINECRAFT WORLD SUPPORT INITIALIZATION ===");

        // Initialize the block state registry for converting Minecraft blocks
        Game::BlockStateRegistry::Initialize();
        Log::Info("✓ Block state registry initialized");

        // Check for common Minecraft world locations
        std::vector<std::string> commonPaths = {
            "world",                    // Current directory
            "../world",                 // Parent directory
            "saves/New World",          // Typical save name
            "saves/World",              // Another common name
            std::string(getenv("HOME") ? getenv("HOME") : "") + "/.minecraft/saves/New World",  // Default Minecraft location
        };

        bool foundWorld = false;
        for (const auto& path : commonPaths) {
            if (path.empty()) continue;

            if (Game::ChunkProvider::LoadMinecraftWorld(path)) {
                Log::Info("✓ Automatically loaded Minecraft world: %s", path.c_str());
                foundWorld = true;
                break;
            }
        }

        if (!foundWorld) {
            Log::Info("No Minecraft world auto-detected, will use procedural generation");
        }

        Log::Info("=== MINECRAFT WORLD SUPPORT READY ===");
        return true;
    }

    bool ProcessWorldArguments(int argc, char* argv[]) {
        for (int i = 1; i < argc - 1; ++i) {
            std::string arg = argv[i];

            if (arg == "--world" || arg == "-w") {
                std::string worldPath = argv[i + 1];
                Log::Info("Loading Minecraft world from command line: %s", worldPath.c_str());

                if (Game::ChunkProvider::LoadMinecraftWorld(worldPath)) {
                    Log::Info("✓ Successfully loaded world: %s", worldPath.c_str());
                    return true;
                } else {
                    Log::Error("✗ Failed to load world: %s", worldPath.c_str());
                    return false;
                }
            }
        }

        return false; // No world argument found
    }

    int Run(int argc, char** argv) {
        // Initialize systems
        Log::Init();
        Log::Info("Starting MyVoxelGame v0.1 with Enhanced Mesher System");

        // **CRITICAL FIX**: Process world arguments and initialize Minecraft support FIRST
        ProcessWorldArguments(argc, argv);
        InitializeMinecraftSupport();

        // **CRITICAL FIX**: Initialize game systems BEFORE any chunk loading
        InitializeGameSystems();

        // Initialize GLFW
        if (!glfwInit()) {
            Log::Error("Failed to initialize GLFW");
            return -1;
        }

        // Setup OpenGL context
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Config::OpenGLMajor);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Config::OpenGLMinor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    #endif
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        // Create window
        GLFWwindow* window = glfwCreateWindow(
            Config::WindowWidth, Config::WindowHeight, Config::WindowTitle,
            nullptr, nullptr
        );
        if (!window) {
            Log::Error("Failed to create GLFW window");
            glfwTerminate();
            return -2;
        }

        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            Log::Error("Failed to initialize GLAD");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -3;
        }

        // Initialize input
        Input::Init(window);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // Log system info
        Log::Info("Vendor: %s", glGetString(GL_VENDOR));
        Log::Info("Renderer: %s", glGetString(GL_RENDERER));
        Log::Info("Version: %s", glGetString(GL_VERSION));

        // Setup debug output
    #ifndef NDEBUG
        if (GLAD_GL_KHR_debug) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(glDebugOutput, nullptr);
            Log::Info("KHR_debug enabled");
        }
    #endif

        // Initialize texture systems
        if (!InitializeTextureSystem()) {
            Log::Error("Failed to initialize texture systems");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -4;
        }

        if (!Render::g_blockHighlight.Initialize()) {
            Log::Error("Failed to initialize block highlight system");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -5;
        }

        // Initialize crosshair with proper asset path
        std::string crosshairPath = GetAssetPath("assets/textures/gui/sprites/hud/crosshair.png");
        if (!Render::g_crosshair.Initialize(crosshairPath)) {
            Log::Warning("Failed to initialize crosshair system, continuing without crosshair");
        }

        // Compile shaders
        Shader blockShader = InitializeShaders();

        // Setup OpenGL state
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glfwSwapInterval(1); // VSync

        // Initialize game systems
        Render::Camera camera;
        camera.position = glm::vec3(0.0f, 80.0f, 0.0f);
        camera.physicsControlled = true;

        // Create World instance (this will set up the global block access)
        Game::World gameWorld;

        // **NEW**: Initialize chunk mesh manager
        g_chunkMeshManager = std::make_unique<ChunkMeshManager>();
        if (!g_chunkMeshManager->Initialize(gameWorld, *Render::g_atlasBuilder, blockShader)) {
            Log::Error("Failed to initialize chunk mesh manager");
            glfwDestroyWindow(window);
            glfwTerminate();
            return -6;
        }

        // Create PlayerController and link it to the world
        Game::PlayerController playerController;
        playerController.SetWorld(&gameWorld);

        // Initialize debug system
        Debug::DebugSystem::Initialize(window);

        // Performance tracking
        Debug::PerformanceMetrics metrics;
        auto frameStartTime = std::chrono::high_resolution_clock::now();

        Log::Info("Entering main render loop with Enhanced Mesher System");

        // MAIN LOOP
        while (!glfwWindowShouldClose(window)) {
            frameStartTime = std::chrono::high_resolution_clock::now();

            // Poll events and update input
            glfwPollEvents();
            Input::UpdateKeyStates();

            // Handle input
            if (Input::IsKeyDown(Input::Key::Escape)) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            bool cursorEnabled = HandleCursorToggle(window, camera);
            HandlePlayerInput(playerController, camera);

            // Update time and metrics
            Time::Tick();
            float dt = static_cast<float>(Time::Delta());
            metrics.AddFrameTimeSample(dt);

            // Update game systems
            camera.Update(dt);
            playerController.Update(dt, camera);

            // **NEW**: Update chunk loading around player
            // Get player chunk position
            glm::vec3 playerPos = playerController.GetPhysics().position;
            int playerChunkX = static_cast<int>(std::floor(playerPos.x / Game::Math::CHUNK_SIZE_X));
            int playerChunkZ = static_cast<int>(std::floor(playerPos.z / Game::Math::CHUNK_SIZE_Z));
            Game::Math::ChunkPos playerChunk{playerChunkX, playerChunkZ};

            // Load chunks around player (view distance of 8 chunks)
            static constexpr int VIEW_DISTANCE = 8;
            static constexpr int UNLOAD_DISTANCE = 12; // Unload beyond 12 chunks

            Game::ChunkProvider::UpdateLoadedChunks(playerChunk, VIEW_DISTANCE);
            Game::ChunkProvider::UnloadDistantChunks(playerChunk, UNLOAD_DISTANCE);

            // **NEW**: Update chunk mesh system
            g_chunkMeshManager->Update(playerController);

            // Start debug frame
            Debug::DebugSystem::BeginFrame();

            // Clear and setup rendering
            glClearColor(0.5f, 0.7f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Calculate matrices
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            float aspect = (height == 0) ? 1.0f : static_cast<float>(width) / static_cast<float>(height);

            glm::mat4 proj = glm::perspective(glm::radians(camera.fov), aspect, 0.01f, 500.0f);
            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 viewProj = proj * view;
            Frustum frustum = Frustum::FromMatrix(viewProj);

            // **NEW**: Render world using the enhanced mesher system
            RenderWorld(playerController, camera, viewProj);

            // Render UI elements
            RenderBlockHighlight(playerController, viewProj);

            // Render crosshair (must be last to appear on top)
            RenderCrosshair(window);

            // **NEW**: Update debug UI with mesh statistics
            int windowWidth, windowHeight;
            glfwGetWindowSize(window, &windowWidth, &windowHeight);

            // Get mesh statistics for debug display
            auto meshStats = g_chunkMeshManager->GetStats();
            metrics.meshesUploadedThisFrame = meshStats.meshesBuiltThisFrame;
            metrics.renderTime = meshStats.meshBuildTimeMs;

            Debug::DebugSystem::RenderDebugUI(
                camera, frustum, playerController, metrics, cursorEnabled,
                windowWidth, windowHeight, width, height
            );

            // Finish debug frame
            Debug::DebugSystem::EndFrame();

            // Swap buffers and reset input
            glfwSwapBuffers(window);
            Input::ResetMouseDelta();
            Input::ResetScrollOffset();

            // Calculate frame time
            auto frameEndTime = std::chrono::high_resolution_clock::now();
            metrics.frameTime = std::chrono::duration<float, std::milli>(frameEndTime - frameStartTime).count();
        }

        // **ENHANCED**: Cleanup with Enhanced Mesher System
        Log::Info("Cleaning up Enhanced Mesher System...");

        // Clean up mesh manager
        g_chunkMeshManager.reset();

        Log::Info("Cleaning up Minecraft world support...");

        // Clear region file cache
        World::RegionFileCache::Instance().Clear();

        // Clear all loaded chunks
        Game::ChunkProvider::ClearAllChunks();

        // Cleanup
        Debug::DebugSystem::Shutdown();

        // Stop all background threads BEFORE cleaning up OpenGL resources
        Log::Info("Stopping background job system...");

        Log::Info("Stopping job system...");
        try {
            JobSystem::g_ThreadPool.Stop();
            Log::Info("Job system stopped successfully");
        } catch (const std::exception& e) {
            Log::Error("Exception stopping job system: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception stopping job system");
        }

        // Clean up OpenGL resources with better error handling
        Log::Info("Cleaning up OpenGL resources...");
        try {
            // Make sure we have a valid OpenGL context
            if (glfwGetCurrentContext() == window) {

                // Clean up global rendering resources
                if (Render::g_atlasBuilder) {
                    Render::g_atlasBuilder.reset();
                }

                // Clear any remaining OpenGL errors
                while (glGetError() != GL_NO_ERROR) {
                    // Clear error queue
                }
            } else {
                Log::Warning("No valid OpenGL context during cleanup - skipping OpenGL resource cleanup");
            }
        } catch (const std::exception& e) {
            Log::Error("Exception during OpenGL cleanup: %s", e.what());
        } catch (...) {
            Log::Error("Unknown exception during OpenGL cleanup");
        }

        Log::Info("Enhanced Voxel Engine shutting down");

        // Destroy window and terminate GLFW with error handling
        try {
            glfwDestroyWindow(window);
            glfwTerminate();
        } catch (...) {
            Log::Error("Exception during GLFW cleanup");
        }

        return 0;
    }

} // namespace PlatformMain