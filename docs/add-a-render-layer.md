Runbook: Adding a New Render Layer

This guide walks through adding a new rendering pass to the three-layer system (opaque → cutout → translucent → new layer), including VBO/IBO layout, mesh generation, and performance budget
updates.

Overview of Current System

The current renderer processes chunks in three passes:
1. Opaque: Solid blocks, front-to-back, Z-buffer enabled
2. Cutout: Alpha-tested blocks, front-to-back, alpha test enabled
3. Translucent: Alpha-blended blocks, back-to-front, blending enabled

Each pass uses separate VBO/IBO data stored in GPUSectionData.

Step 1: Define New Layer Purpose

Example: Emissive Layer

Let's add an Emissive layer for light-emitting blocks that need special rendering:
- Purpose: Render glowing blocks (torches, lava, glowstone) with bloom effect
- Rendering: After translucent pass, additive blending, no depth write
- Optimization: Skip during shadow pass, render to separate framebuffer for bloom

Determine Layer Properties

    // New layer characteristics
    enum class RenderLayer {
        OPAQUE = 0,
        CUTOUT = 1,
        TRANSLUCENT = 2,
        EMISSIVE = 3,        // New layer
        LAYER_COUNT = 4
    };
    
    struct EmissiveLayerProperties {
        bool depthWrite = false;        // Don't write to depth buffer
        bool depthTest = true;          // Still test depth
        BlendMode blendMode = ADDITIVE; // Add to existing color
        CullMode cullMode = NONE;       // Don't cull faces
        float glowIntensity = 1.0f;     // Brightness multiplier
    };

Step 2: Update GPU Data Structure

Extend GPUSectionData

    // In src/client/renderer/mesh/ClientMeshManager.hpp
    struct GPUSectionData {
        // Existing layers
        GLuint opaqueVBO = 0;
        GLuint opaqueIBO = 0;
        GLsizei opaqueIndexCount = 0;

        GLuint cutoutVBO = 0;
        GLuint cutoutIBO = 0;
        GLsizei cutoutIndexCount = 0;

        GLuint translucentVBO = 0;
        GLuint translucentIBO = 0;
        GLsizei translucentIndexCount = 0;

        // NEW: Emissive layer
        GLuint emissiveVBO = 0;
        GLuint emissiveIBO = 0;
        GLsizei emissiveIndexCount = 0;

        // Layer state flags
        bool hasOpaqueGeometry = false;
        bool hasCutoutGeometry = false;
        bool hasTranslucentGeometry = false;
        bool hasEmissiveGeometry = false;  // New flag
    };

Update VBO/IBO Layout

    // Emissive vertex structure (enhanced with glow data)
    struct EmissiveVertex {
        glm::vec3 position;      // World position
        glm::vec3 normal;        // Surface normal
        glm::vec2 texCoord;      // Texture coordinates  
        uint16_t textureId;      // Atlas texture index
        uint16_t blockLight;     // Block light level (0-15)
        uint16_t skyLight;       // Sky light level (0-15)
        float glowIntensity;     // Glow strength (0.0-2.0)
        glm::vec3 glowColor;     // RGB glow color
    
        // Total: 48 bytes per vertex
    };
    
    // Index buffer remains same format (uint32_t indices)

Step 3: Update Mesh Generation

Extend MeshBuildResult
    
    // In src/common/network/PacketTypes.hpp - MeshBuildResult
    struct MeshBuildResult {
        ChunkPos chunkPos;
        int sectionY;

        // Existing layer data
        SectionMeshData opaque;
        SectionMeshData cutout;
        SectionMeshData translucent;

        // NEW: Emissive layer data  
        SectionMeshData emissive;

        // Success flags
        bool hasOpaque = false;
        bool hasCutout = false;
        bool hasTranslucent = false;
        bool hasEmissive = false;    // New flag
    };
    
    struct SectionMeshData {
        std::vector<uint8_t> vertices;  // Raw vertex data (any format)
        std::vector<uint32_t> indices;  // Index data
        size_t vertexSize = 0;          // Bytes per vertex
        size_t vertexCount = 0;         // Number of vertices
        size_t indexCount = 0;          // Number of indices
    };

Update Mesh Building in Worker Thread

    // In ClientWorkerPool mesh building
    MeshBuildResult BuildSectionMesh(const MeshBuildJob& job) {
        auto result = MeshBuildResult{};
        result.chunkPos = job.chunkPos;
        result.sectionY = job.sectionY;

        // Build existing layers
        result.opaque = BuildOpaqueLayer(job.chunkData, job.sectionY);
        result.cutout = BuildCutoutLayer(job.chunkData, job.sectionY);
        result.translucent = BuildTranslucentLayer(job.chunkData, job.sectionY);

        // NEW: Build emissive layer
        result.emissive = BuildEmissiveLayer(job.chunkData, job.sectionY);
        result.hasEmissive = result.emissive.vertexCount > 0;

        return result;
    }
    
    // New layer building function
    SectionMeshData BuildEmissiveLayer(std::shared_ptr<Chunk> chunk, int sectionY) {
        SectionMeshData meshData;
        std::vector<EmissiveVertex> vertices;
        std::vector<uint32_t> indices;

        int startY = sectionY * 16;
        int endY = startY + 16;

        for (int y = startY; y < endY; y++) {
            for (int x = 0; x < 16; x++) {
                for (int z = 0; z < 16; z++) {
                    BlockID blockId = chunk->GetBlock(x, y, z);

                    // Only process emissive blocks
                    if (!IsEmissiveBlock(blockId)) {
                        continue;
                    }

                    auto blockModel = GetBlockModel(blockId);
                    auto emissiveProps = GetEmissiveProperties(blockId);

                    // Generate faces for emissive block
                    for (auto face : {NORTH, SOUTH, EAST, WEST, UP, DOWN}) {
                        if (ShouldRenderFace(chunk, x, y, z, face)) {
                            AddEmissiveFace(vertices, indices, x, y, z, face,
                                        blockModel, emissiveProps);
                        }
                    }
                }
            }
        }

        // Pack data into SectionMeshData
        meshData.vertices = PackVertices(vertices);
        meshData.indices = indices;
        meshData.vertexSize = sizeof(EmissiveVertex);
        meshData.vertexCount = vertices.size();
        meshData.indexCount = indices.size();

        return meshData;
    }

Step 4: Update GPU Upload System

Extend Upload Methods

    // In ClientMeshManager::UploadSectionMesh()
    void ClientMeshManager::UploadSectionMesh(ChunkPos chunkPos, int sectionY,
    const MeshBuildResult::SectionMeshData& meshData) {
        auto key = SectionKey{chunkPos, sectionY};
        auto& gpuData = m_gpuData[key];

        // Upload existing layers...

        // NEW: Upload emissive layer
        if (meshData.hasEmissive) {
            UploadEmissiveLayer(gpuData, meshData.emissive);
        }
    }
    
    void ClientMeshManager::UploadEmissiveLayer(GPUSectionData& gpuData,
    const SectionMeshData& emissiveData) {
        // Generate VBO/IBO if needed
        if (gpuData.emissiveVBO == 0) {
            glGenBuffers(1, &gpuData.emissiveVBO);
            glGenBuffers(1, &gpuData.emissiveIBO);
        }

        // Upload vertex data
        glBindBuffer(GL_ARRAY_BUFFER, gpuData.emissiveVBO);
        glBufferData(GL_ARRAY_BUFFER, emissiveData.vertices.size(),
                   emissiveData.vertices.data(), GL_STATIC_DRAW);

        // Upload index data  
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpuData.emissiveIBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   emissiveData.indices.size() * sizeof(uint32_t),
                   emissiveData.indices.data(), GL_STATIC_DRAW);

        gpuData.emissiveIndexCount = emissiveData.indexCount;
        gpuData.hasEmissiveGeometry = true;

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log::Warning("Emissive layer upload failed: OpenGL error %d", error);
            gpuData.hasEmissiveGeometry = false;
        }
    }

Update Cleanup Methods

    // In ClientMeshManager::RemoveSectionGPUData()
    void ClientMeshManager::RemoveSectionGPUData(ChunkPos chunkPos, int sectionY) {
        auto key = SectionKey{chunkPos, sectionY};
        auto it = m_gpuData.find(key);

        if (it != m_gpuData.end()) {
            const auto& data = it->second;

            // Clean up existing layers...

            // NEW: Clean up emissive layer
            if (data.emissiveVBO != 0) {
                glDeleteBuffers(1, &data.emissiveVBO);
                glDeleteBuffers(1, &data.emissiveIBO);
            }

            m_gpuData.erase(it);
        }
    }

Step 5: Update Renderer

Add Render Pass
    
    // In ChunkRenderer::RenderChunks()
    void ChunkRenderer::RenderChunks(const std::vector<VisibleSection>& sections,
    const RenderContext& context) {
        // Existing passes
        RenderOpaquePass(sections, context);
        RenderCutoutPass(sections, context);
        RenderTranslucentPass(sections, context);

        // NEW: Emissive pass
        RenderEmissivePass(sections, context);
    }
    
    void ChunkRenderer::RenderEmissivePass(const std::vector<VisibleSection>& sections,
    const RenderContext& context) {
        // Set emissive render state
        glDepthMask(GL_FALSE);              // No depth write
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);        // Additive blending
        glDisable(GL_CULL_FACE);            // No face culling

        // Bind emissive shader
        m_emissiveShader->Use();
        m_emissiveShader->SetMatrix4("viewMatrix", context.viewMatrix);
        m_emissiveShader->SetMatrix4("projMatrix", context.projMatrix);
        m_emissiveShader->SetFloat("time", context.gameTime);

        // Render sections with emissive geometry
        for (const auto& section : sections) {
            const auto* gpuData = GetSectionGPUData(section.chunkPos, section.sectionY);

            if (gpuData && gpuData->hasEmissiveGeometry) {
                RenderEmissiveSection(section, *gpuData, context);
            }
    }

        // Restore render state
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
    }

Emissive Section Rendering

    void ChunkRenderer::RenderEmissiveSection(const VisibleSection& section,
    const GPUSectionData& gpuData,
    const RenderContext& context) {
        // Bind vertex data
        glBindBuffer(GL_ARRAY_BUFFER, gpuData.emissiveVBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpuData.emissiveIBO);

        // Setup vertex attributes for EmissiveVertex
        glEnableVertexAttribArray(0); // position
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(EmissiveVertex),
                           reinterpret_cast<void*>(offsetof(EmissiveVertex, position)));

        glEnableVertexAttribArray(1); // normal
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(EmissiveVertex),
                           reinterpret_cast<void*>(offsetof(EmissiveVertex, normal)));

        glEnableVertexAttribArray(2); // texCoord
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(EmissiveVertex),
                           reinterpret_cast<void*>(offsetof(EmissiveVertex, texCoord)));

        glEnableVertexAttribArray(3); // glowIntensity
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(EmissiveVertex),
                           reinterpret_cast<void*>(offsetof(EmissiveVertex, glowIntensity)));

        glEnableVertexAttribArray(4); // glowColor
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(EmissiveVertex),
                           reinterpret_cast<void*>(offsetof(EmissiveVertex, glowColor)));

        // Set section-specific uniforms
        glm::vec3 sectionOffset(section.chunkPos.x * 16, section.sectionY * 16, section.chunkPos.z * 16);
        m_emissiveShader->SetVector3("sectionOffset", sectionOffset);

        // Draw
        glDrawElements(GL_TRIANGLES, gpuData.emissiveIndexCount, GL_UNSIGNED_INT, nullptr);

        // Update stats
        m_stats.emissiveDrawCalls++;
        m_stats.emissiveVerticesRendered += gpuData.emissiveIndexCount;
    }

Step 6: Create Emissive Shader

Vertex Shader (emissive.vert)

    #version 330 core
    
    layout (location = 0) in vec3 position;
    layout (location = 1) in vec3 normal;
    layout (location = 2) in vec2 texCoord;
    layout (location = 3) in float glowIntensity;
    layout (location = 4) in vec3 glowColor;
    
    uniform mat4 viewMatrix;
    uniform mat4 projMatrix;
    uniform vec3 sectionOffset;
    uniform float time;
    
    out vec2 fragTexCoord;
    out vec3 fragGlowColor;
    out float fragGlowIntensity;
    out float fragPulse;
    
    void main() {
        vec3 worldPos = position + sectionOffset;
        gl_Position = projMatrix * viewMatrix * vec4(worldPos, 1.0);

        fragTexCoord = texCoord;
        fragGlowColor = glowColor;
        fragGlowIntensity = glowIntensity;

        // Animated pulsing effect
        fragPulse = 0.8 + 0.2 * sin(time * 2.0);
    }
    
    Fragment Shader (emissive.frag)
    
    #version 330 core
    
    in vec2 fragTexCoord;
    in vec3 fragGlowColor;
    in float fragGlowIntensity;
    in float fragPulse;
    
    uniform sampler2D textureAtlas;
    
    out vec4 fragColor;
    
    void main() {
        // Sample base texture
        vec4 texColor = texture(textureAtlas, fragTexCoord);

        if (texColor.a < 0.1) {
            discard; // Alpha test
        }

        // Apply glow effect
        vec3 emissiveColor = texColor.rgb * fragGlowColor * fragGlowIntensity * fragPulse;

        // Output additive color
        fragColor = vec4(emissiveColor, texColor.a);
    }

Step 7: Update Performance Budgets

Extend Statistics

    // In ClientMeshManager::ClientMeshStats
    struct ClientMeshStats {
        // Existing stats...

        // NEW: Emissive layer stats
        std::atomic<size_t> emissiveMeshBuilds{0};
        std::atomic<size_t> emissiveUploads{0};
        std::atomic<size_t> emissiveVertices{0};
        std::atomic<size_t> emissiveIndices{0};
    };
    
    // In ChunkRenderer::RenderStats  
    struct RenderStats {
        // Existing stats...

        // NEW: Emissive rendering stats
        size_t emissiveDrawCalls = 0;
        size_t emissiveVerticesRendered = 0;
        float emissiveRenderTime = 0.0f;
    };

Update Performance Budgets

    // In ClientMeshManager configuration
    struct ClientMeshConfig {
        int maxMeshBuildsPerFrame = 5;          // Unchanged
        int maxGPUUploadsPerFrame = 3;          // Reduce from 4 to accommodate new layer
        float meshBuildBudgetMs = 1.2f;         // Increase slightly (was 1.0f)
        float gpuUploadBudgetMs = 3.0f;         // Increase (was 2.0f)

        // NEW: Emissive-specific limits
        int maxEmissiveUploadsPerFrame = 1;     // Limit emissive uploads
        float emissiveRenderBudgetMs = 0.5f;    // Emissive pass budget
    };
    
    // In frame processing
    void ClientMeshManager::PerformGPUUploads() {
        auto frameStart = std::chrono::steady_clock::now();
        const float budgetMs = m_config.gpuUploadBudgetMs;

        int regularUploads = 0;
        int emissiveUploads = 0;

        while (HasPendingUploads()) {
            auto elapsed = GetElapsedMs(frameStart);
            if (elapsed > budgetMs) break;

            auto result = GetNextUploadCandidate();

            // Prioritize regular geometry over emissive
            if (result.hasEmissive && emissiveUploads >= m_config.maxEmissiveUploadsPerFrame) {
                continue; // Skip emissive upload this frame
            }

            UploadSectionMesh(result.chunkPos, result.sectionY, result);

            if (result.hasEmissive) {
                emissiveUploads++;
            }
            regularUploads++;
        }
    }

Step 8: Update Block Registry

Add Emissive Block Properties

    // In BlockRegistry system
    enum class BlockProperty {
    // Existing properties...
        EMISSIVE,           // Block emits light
        GLOW_INTENSITY,     // How bright the glow effect
        GLOW_COLOR,         // RGB color of glow
    };
    
    // Block definitions
    void RegisterEmissiveBlocks() {
        // Torch
        RegisterBlock(BlockID::Torch, {
            {BlockProperty::EMISSIVE, true},
            {BlockProperty::GLOW_INTENSITY, 1.5f},
            {BlockProperty::GLOW_COLOR, glm::vec3(1.0f, 0.8f, 0.4f)} // Warm orange
        });

      // Lava
      RegisterBlock(BlockID::Lava, {
          {BlockProperty::EMISSIVE, true},
          {BlockProperty::GLOW_INTENSITY, 2.0f},
          {BlockProperty::GLOW_COLOR, glm::vec3(1.0f, 0.3f, 0.0f)} // Bright red
      });

      // Glowstone
      RegisterBlock(BlockID::Glowstone, {
          {BlockProperty::EMISSIVE, true},
          {BlockProperty::GLOW_INTENSITY, 1.0f},
          {BlockProperty::GLOW_COLOR, glm::vec3(1.0f, 1.0f, 0.7f)} // Soft yellow
      });
    }

Step 9: Testing and Validation

Performance Testing

    // Measure emissive layer impact
    void ProfileEmissiveRendering() {
        auto startTime = std::chrono::high_resolution_clock::now();

        RenderEmissivePass(visibleSections, context);
    
        auto endTime = std::chrono::high_resolution_clock::now();
        float emissiveTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    
        if (emissiveTime > m_config.emissiveRenderBudgetMs) {
            Log::Warning("Emissive pass exceeded budget: %.2fms (limit: %.2fms)",
                emissiveTime, m_config.emissiveRenderBudgetMs);
        }
    
        m_stats.emissiveRenderTime = emissiveTime;
    }

Visual Validation

- Create test world with various emissive blocks
- Verify glow effects render correctly
- Test performance with many emissive blocks
- Ensure proper blending with other geometry

Memory Impact Analysis

    // Calculate memory overhead per emissive vertex
    size_t emissiveVertexSize = sizeof(EmissiveVertex);        // 48 bytes
    size_t standardVertexSize = sizeof(StandardVertex);       // 32 bytes  
    size_t additionalBytes = emissiveVertexSize - standardVertexSize; // 16 bytes
    
    Log::Info("Emissive layer adds %zu bytes per vertex", additionalBytes);
    
    // Estimate total memory impact
    size_t totalEmissiveVertices = GetTotalEmissiveVertices();
    size_t additionalMemory = totalEmissiveVertices * additionalBytes;
    Log::Info("Total additional memory: %.2f MB", additionalMemory / (1024.0f * 1024.0f));

Adding a new render layer requires careful coordination between mesh generation, GPU upload, and rendering systems, with particular attention to performance budgets and memory usage. The emissive
layer example demonstrates the complete process while maintaining the established architecture patterns.
