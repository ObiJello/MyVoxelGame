Meshing Pipeline

Pipeline Overview

The meshing pipeline converts block data into GPU-ready vertex/index buffers through a multi-stage process involving background workers and main thread coordination.

graph TD
A[Loaded Chunk] --> B{Needs Mesh?}
B -->|Yes| C[Schedule Build]
B -->|No| D[Skip]

      C --> E[ClientWorkerPool Queue]
      E --> F[Worker Thread: Greedy Meshing]
      F --> G[Generate Vertices/Indices]
      G --> H[Submit MeshBuildResult]

      H --> I[ClientMeshManager Queue]
      I --> J[Main Thread: Process Results]
      J --> K[Upload to GPU]
      K --> L[Ready for Rendering]

      classDef worker fill:#e1f5fe
      classDef mainThread fill:#f3e5f5
      classDef gpu fill:#fff3e0

      class E,F,G,H worker
      class I,J mainThread
      class K,L gpu

Worker Thread Processing

Job Submission

    // In ClientMeshManager::ScheduleSectionMeshBuild()
    Threading::SubmitClientMeshBuild({
        .chunkPos = chunkPos,
        .sectionY = sectionY,
        .priority = CalculateMeshPriority(chunkPos, sectionY),
        .chunkData = clientChunk->chunkData
    });

Mesh Build Execution

Worker threads execute the CPU-intensive greedy meshing algorithm (Note: This is an optimization; vanilla Minecraft's chunk tesselator emits individual model quads with ambient occlusion rather than merged faces):

    // In ClientWorkerPool mesh building
    Network::MeshBuildResult BuildSectionMesh(const MeshBuildJob& job) {
        auto result = Network::MeshBuildResult{};
        result.chunkPos = job.chunkPos;
        result.sectionY = job.sectionY;

        // Generate geometry for each render layer
        result.opaque = BuildOpaqueLayer(job.chunkData, job.sectionY);
        result.cutout = BuildCutoutLayer(job.chunkData, job.sectionY);
        result.translucent = BuildTranslucentLayer(job.chunkData, job.sectionY);
        
        return result;
    }

Render Layer Separation

Each section generates separate mesh data for different rendering passes:

Note: We use 3 render layers (opaque/cutout/translucent) vs vanilla's 4 layers (SOLID/CUTOUT_MIPPED/CUTOUT/TRANSLUCENT).

Opaque Layer - Solid blocks (stone, dirt, wood)
- Vertices: Position, normal, UV, texture atlas index
- Rendering: Front-to-back with Z-buffer
- Optimization: Aggressive face culling, greedy meshing

Cutout Layer - Blocks with alpha testing (grass, leaves, glass panes)
- Vertices: Same format as opaque + alpha threshold
- Rendering: Front-to-back with alpha test
- Optimization: Limited face culling (preserve silhouettes)

Translucent Layer - Blocks with blending (water, stained glass)
- Vertices: Same format + alpha blending factor
- Rendering: Back-to-front with alpha blending
- Optimization: No face culling, manual sorting

Result Processing and Upload

Result Queue Processing

    // In ClientMeshManager::ProcessMeshBuildResults() (called per frame)
    void ProcessMeshBuildResults() {
        auto results = GetMeshResultQueue().DrainAll();

        for (const auto& result : results) {
            if (ValidateMeshBuildResult(result)) {
                UploadMeshResultToGPU(result.chunkPos, result.sectionY, result);
            }
        }
    }

GPU Upload with Time Budget

    // In ClientMeshManager::PerformGPUUploads()
    void PerformGPUUploads() {
        auto frameStart = std::chrono::steady_clock::now();
        const float budgetMs = m_config.gpuUploadBudgetMs; // 2-3ms target

        while (HasPendingUploads()) {
            auto elapsed = GetElapsedMs(frameStart);
            if (elapsed > budgetMs) break; // Respect time budget

            auto result = GetNextUploadCandidate();
            UploadSectionMeshToGPU(result);

            m_stats.meshUploadedToGPU++;
        }
    }
    
VBO/IBO Upload Format
    
    struct GPUSectionData {
        // Opaque geometry
        GLuint opaqueVBO = 0;
        GLuint opaqueIBO = 0;
        GLsizei opaqueIndexCount = 0;

        // Cutout geometry  
        GLuint cutoutVBO = 0;
        GLuint cutoutIBO = 0;
        GLsizei cutoutIndexCount = 0;

        // Translucent geometry
        GLuint translucentVBO = 0;
        GLuint translucentIBO = 0;
        GLsizei translucentIndexCount = 0;

        // Metadata
        bool hasOpaqueGeometry = false;
        bool hasCutoutGeometry = false;
        bool hasTranslucentGeometry = false;
    };
    
Prioritization and Scheduling
    
Distance-Based Priority
    
    float CalculateMeshPriority(ChunkPos chunkPos, int sectionY) const {
        glm::vec3 sectionCenter = ChunkToWorldPos(chunkPos, sectionY);
        float distance = glm::distance(sectionCenter, m_playerPosition);

        // Higher priority for closer sections
        return 1.0f / (1.0f + distance * 0.01f);
    }

Build Scheduling Logic

1. High Priority (0-64 blocks): Submit immediately, multiple per frame
2. Medium Priority (64-128 blocks): Submit 1-2 per frame
3. Low Priority (128+ blocks): Submit during idle frames only

Cancellation on Chunk Unload

    void CancelMeshJobs(ChunkPos chunkPos) {
        // Cancel pending jobs in worker queue
        Threading::CancelClientMeshBuilds(chunkPos);

        // Remove completed results not yet uploaded
        GetMeshResultQueue().RemoveResultsFor(chunkPos);
    }

Performance Budgets and Limits

Per-Frame Limits

- Mesh builds scheduled: 5 per frame max
- GPU uploads: 3 per frame max
- Scheduling time budget: 1ms per frame
- Upload time budget: 2-3ms per frame

Queue Bounds

- Worker job queue: 256 jobs max (prevent memory growth)
- Result queue: 128 results max (balance latency vs memory)
- GPU upload queue: 64 pending uploads max

Memory Management

    // Clean up GPU resources when chunk unloads
    void RemoveSectionGPUData(ChunkPos chunkPos, int sectionY) {
        auto key = SectionKey{chunkPos, sectionY};
        auto it = m_gpuData.find(key);

        if (it != m_gpuData.end()) {
            const auto& data = it->second;

            // Free OpenGL resources
            glDeleteBuffers(1, &data.opaqueVBO);
            glDeleteBuffers(1, &data.opaqueIBO);
            // ... free other layer VBOs/IBOs

             m_gpuData.erase(it);
        }
    }

Integration Points

ClientChunkManager Interface

    // Query chunk data for meshing
    const ClientChunk* chunk = g_clientChunkManager->GetChunk(chunkPos);
    if (chunk && chunk->IsLoaded()) {
        // Check dirty sections
        for (int sectionY : chunk->dirtySections) {
            ScheduleSectionMeshBuild(chunkPos, sectionY);
        }
    }

ChunkRenderer Interface

    // Query GPU data for rendering
    const GPUSectionData* data = g_clientMeshManager->GetSectionGPUData(chunkPos, sectionY);
    if (data && data->hasOpaqueGeometry) {
        RenderOpaqueSection(data);
    }

Worker Pool Coordination

- Job submission: Threading::SubmitClientMeshBuild()
- Result retrieval: ClientMeshManager::GetMeshResultQueue()
- Cancellation: Threading::CancelClientMeshBuilds()

The meshing pipeline balances throughput, latency, and resource usage to maintain smooth gameplay while efficiently converting block data into renderable geometry.

Implementation files:
- src/client/renderer/mesh/ClientMeshManager.cpp
- src/client/world/ClientWorkerPool.cpp
- src/client/renderer/mesh/Mesher.cpp
