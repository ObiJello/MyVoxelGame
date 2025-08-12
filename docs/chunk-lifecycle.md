Chunk Section Lifecycle

Section State Machine

The client-side chunk sections follow a simplified but well-defined lifecycle from network reception to GPU rendering.

stateDiagram-v2
[*] --> UNLOADED: Initial state

      UNLOADED --> LOADED: ChunkDataS2CPacket received

      LOADED --> MESH_SCHEDULED: Schedule mesh build
      MESH_SCHEDULED --> MESHING: Worker picks up job
      MESHING --> MESH_COMPLETE: Worker completes build
      MESH_COMPLETE --> GPU_UPLOAD: Main thread processes result
      GPU_UPLOAD --> READY: VBO/IBO uploaded

      READY --> DIRTY: BlockChangeS2CPacket
      DIRTY --> MESH_SCHEDULED: Schedule rebuild

      READY --> UNLOADING: UnloadChunkS2CPacket
      MESH_SCHEDULED --> UNLOADING: Cancel mesh build
      MESHING --> UNLOADING: Cancel in-progress work
      MESH_COMPLETE --> UNLOADING: Discard result
      GPU_UPLOAD --> UNLOADING: Cancel upload
      DIRTY --> UNLOADING: Clear dirty flags

      UNLOADING --> UNLOADED: GPU cleanup complete
      UNLOADED --> [*]: Memory freed

      note right of LOADED: Chunk data available
      note right of MESHING: CPU mesh generation
      note right of READY: Available for rendering
      note right of DIRTY: Needs mesh rebuild

State Definitions

UNLOADED

- Description: Section has no data on client
- Memory usage: 0 bytes
- Rendering: Section not drawn
- Transitions: → LOADED (on packet)

LOADED

- Description: Block data received from server
- Memory usage: ~8KB per section (16×16×16 × 2 bytes per block state)
- Data: Block IDs, light data (~4KB), biome info
- Total per section: ~12-13KB including lighting
- Transitions: → MESH_SCHEDULED (automatic)

MESH_SCHEDULED

- Description: Mesh build job queued for worker
- Queue: Pending in ClientWorkerPool job queue
- Priority: Based on distance from player
- Transitions: → MESHING (worker pickup), → UNLOADING (cancel)

MESHING

- Description: Worker thread generating mesh data
- Process: Greedy meshing algorithm running on CPU
- Output: Separate vertex/index arrays for 3 render layers
- Transitions: → MESH_COMPLETE (success), → UNLOADING (cancel)

MESH_COMPLETE

- Description: Mesh build finished, awaiting GPU upload
- Queue: Result in MeshBuildResult queue
- Memory: CPU vertex/index buffers allocated
- Transitions: → GPU_UPLOAD (main thread pickup)

GPU_UPLOAD

- Description: Uploading mesh data to VBO/IBO
- Time budget: 2-3ms per frame limit
- Process: glBufferData() calls for each layer
- Transitions: → READY (upload complete)

READY

- Description: Section ready for rendering
- GPU data: VBOs/IBOs allocated and populated
- Rendering: Section drawn in appropriate render passes
- Transitions: → DIRTY (block change), → UNLOADING (chunk unload)

DIRTY

- Description: Block data changed, mesh needs rebuild
- Trigger: BlockChangeS2CPacket received
- Optimization: Multiple changes batched together
- Transitions: → MESH_SCHEDULED (rebuild)

UNLOADING

- Description: Cleaning up all section resources
- GPU cleanup: Delete VBOs/IBOs via glDeleteBuffers()
- Memory cleanup: Free CPU mesh data and chunk data
- Transitions: → UNLOADED (cleanup complete)

State Management Code

State Transitions

    // In ClientChunkManager::ProcessChunkDataS2CPacket()
    void ProcessChunkDataS2CPacket(const ChunkDataS2CPacket& packet) {
        auto clientChunk = std::make_unique<ClientChunk>(ChunkPos{packet.chunkX, packet.chunkZ});
        clientChunk->chunkData = DeserializeChunkData(packet);
        clientChunk->state = ChunkState::LOADED;

        // Automatically schedule mesh builds
        g_clientMeshManager->ScheduleChunkMeshBuilds(clientChunk->position);
    }

    // In ClientMeshManager::ScheduleSectionMeshBuild()  
    void ScheduleSectionMeshBuild(ChunkPos chunkPos, int sectionY, float priority) {
        // LOADED → MESH_SCHEDULED
        Threading::SubmitClientMeshBuild({
            .chunkPos = chunkPos,
            .sectionY = sectionY,
            .priority = priority
        });
    }
    
    State Queries
    
    // Check if section is ready for rendering
    bool IsSectionReady(ChunkPos chunkPos, int sectionY) {
        const ClientChunk* chunk = g_clientChunkManager->GetChunk(chunkPos);
        if (!chunk || !chunk->IsLoaded()) return false;

        const GPUSectionData* gpuData = g_clientMeshManager->GetSectionGPUData(chunkPos, sectionY);
        return gpuData != nullptr;
    }

Lifecycle Timing

Typical Section Load Time

1. Network reception: 1-5ms (packet decode)
2. Mesh scheduling: <1ms (job queue submission)
3. Worker queue time: 0-50ms (depends on load)
4. Mesh building: 10-30ms (CPU greedy meshing)
5. Result queue time: 0-16ms (until next frame)
6. GPU upload: 1-3ms (VBO/IBO creation)
7. Total time: 15-100ms from packet to renderable

Performance Targets

- Mesh build rate: 50-100 sections/second across all workers
- GPU upload rate: 20-30 sections/second on main thread
- Memory per section: ~12KB blocks + 10-50KB mesh data + 5-20KB GPU
- Concurrent sections: 1000-2000 sections in READY state typical

Error Handling and Recovery

Mesh Build Failures

    // In worker thread mesh building
    try {
        auto result = BuildSectionMesh(job);
        g_clientMeshManager->GetMeshResultQueue().try_push(std::move(result));
    } catch (const std::exception& e) {
        Log::Warning("Mesh build failed for section (%d,%d,%d): %s",
        job.chunkPos.x, job.chunkPos.z, job.sectionY, e.what());
        // Section remains in MESH_SCHEDULED state, will retry later
    }
    
    GPU Upload Failures
    
    // In main thread GPU upload
    void UploadSectionMesh(const MeshBuildResult& result) {
        GLuint vbo, ibo;
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);

        // Upload with error checking
        glBufferData(GL_ARRAY_BUFFER, result.vertices.size(), result.vertices.data(), GL_STATIC_DRAW);
        if (glGetError() != GL_NO_ERROR) {
            Log::Warning("VBO upload failed for section (%d,%d,%d)",
                       result.chunkPos.x, result.chunkPos.z, result.sectionY);
            glDeleteBuffers(1, &vbo);
            glDeleteBuffers(1, &ibo);
            // Section remains in MESH_COMPLETE state, will retry next frame
            return;
        }
    }

Cancellation During Unload

    // In ClientChunkManager::UnloadChunk()
    void UnloadChunk(ChunkPos chunkPos) {
        // Cancel mesh builds for all sections
        for (int sectionY = 0; sectionY < 24; ++sectionY) {
            Threading::CancelClientMeshBuild(chunkPos, sectionY);
            g_clientMeshManager->RemoveSectionGPUData(chunkPos, sectionY);
        }

        // Remove chunk data
        m_chunks.erase(chunkPos);
    }

Integration Points

Network Thread

- Triggers UNLOADED → LOADED via ProcessChunkDataS2CPacket()
- Triggers READY → DIRTY via ProcessBlockChange()
- Triggers any state → UNLOADING via ProcessChunkUnloadPacket()

Worker Threads

- Process MESH_SCHEDULED → MESHING → MESH_COMPLETE
- Handle cancellation during MESHING state

Main Thread

- Manages MESH_COMPLETE → GPU_UPLOAD → READY
- Enforces time budgets for GPU uploads
- Queries section state for rendering decisions

The section lifecycle ensures efficient resource usage while maintaining responsive world updates and smooth rendering performance.
