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

## Leaves, graphics mode, and the alpha-free opaque pass (2026-08-30)

Measured on Vulkan at RD32 the cutout pass was ~2.2 ms and almost entirely
leaves: every leaf block is non-occluding, so a canopy of N leaves emitted ~6N
quads. Three changes, all in the mesher's per-thread `CachedBlockProps` and
the model loader — the renderer and shaders did not grow any new state.

**Graphics: Fast / Fancy (`graphicsMode`, 0 = Fast, 1 = Fancy, default Fancy).**
This is MC's `cutoutLeaves` option (the modern name of the old Fast/Fancy
split; `Options.java "options.cutoutLeaves"`, `LeavesBlock.setCutoutLeaves`),
implemented exactly:

- `ItemBlockRenderTypes.getChunkRenderType`: a LeavesBlock is CUTOUT when
  cutoutLeaves, SOLID otherwise. → `CachedBlockProps::renderLayer` is forced to
  Opaque for `isLeaves` blocks under Fast.
- `LeavesBlock.skipRendering`: `!cutoutLeaves && neighbour instanceof
  LeavesBlock` → skip the face. → `skipAgainstLeaves`, tested next to
  `cullsAgainstSelf` in `ProcessBlock`, against ANY of the 11 leaf types.
- Vanilla leaves are `noOcclusion()` in every mode, so `isOpaque` does NOT
  change: a stone face behind a Fast leaf is still drawn and AO still treats
  the leaf as open. Only the layer and the skip move.

**Cull Leaves (`cullLeaves`, default false).** Engine-only, no MC equivalent
(the "Cull Leaves" mod): under Fancy, apply the leaf-neighbour skip while
keeping leaves cutout and non-occluding. Default off so Fancy stays MC-exact.
Under Fast it is redundant.

**How the option reaches worker threads.** `Mesher::SetLeafOptions` (main
thread) stores a packed byte and bumps `s_leafOptionsGeneration` with release
semantics; `EnsureBlockPropsCache` loads the generation with acquire on every
section build and rebuilds its thread-local props table when it differs from
the one it was built for. Workers never touch `GameSettings`.
`Mesher::SyncLeafOptionsFromSettings` reads `graphicsMode`/`cullLeaves` from
`Platform::g_gameSettings` and publishes them; it returns true when anything
changed, and `OptionsScreens::ApplyLeafOptions` then dirties every section that
has GPU data (`ClientMeshManager::ForEachActiveSection` →
`ClientChunkManager::MarkSectionDirty`) — the engine's equivalent of MC's
`LevelRenderer.allChanged()`. It must also be called once at startup after the
settings load, or a saved `graphicsMode:0` is not honoured until the option is
touched.

**Opaque pass has no alpha test.** `shaders/block_opaque.frag` and
`block_opaque_vk.frag` used to `discard` at alpha < 0.1 for one reason:
grass_block's side-overlay quads were in the opaque layer. MC's solid layer has
no discard; it puts GRASS_BLOCK whole into CUTOUT instead. That costs the fill
of every grass top and side in the alpha-tested pass, so this engine does it per
FACE: `FaceDef::cutoutOverlay` is set at model resolution
(`BlockModelRegistry::AnnotateFaceLayers`, run on every model entering the
registry — resolved, rotated and merged) for any face whose resolved sprite
ends in `_overlay`, and `ProcessBlock` routes such a face of an Opaque block to
the cutout layer. grass_block stays Opaque and occluding; its overlay quads keep
their cullface, tintindex 0 and back-face culling, so lighting/AO/tint are
unchanged — only the vertex list they land in differs.

With the discard gone, every Opaque block whose model samples transparent
texels had to be audited (script: resolve each Opaque block's blockstates →
models → sprites, check PNG alpha, cross-check against MC's
`ItemBlockRenderTypes.TYPE_BY_BLOCK`). 32 blocks were registered Opaque that
MC renders in cutout — cactus, big_dripleaf, sculk_sensor,
calibrated_sculk_sensor, sculk_shrieker, stonecutter, comparator, repeater and
the 24 copper bars/chains/lanterns — and now are Cutout in `BlockDefs.inc`.
None is a full cube, so occlusion is unchanged. Blocks whose sprites have
transparent pixels only OUTSIDE the sampled sub-rect (anvil_top, cake_side)
are solid in MC too and were left alone.

Invariant going forward: nothing in the opaque layer may depend on an alpha
test. A block that looks wrong there is in the wrong layer — fix
`BlockDefs.inc`, do not put the discard back.

## Greedy face merging (2026-08)

**Why.** The 750-blocks-up look-down view was triangle-volume-bound: ~19,400
visible sections, 25 ms GPU frame with opaque alone at 17 ms, and submission
already minimized (merged multi-draws). The only lever left was fewer quads
for the same pixels, so coplanar identical block faces are now merged into
maximal rectangles — one quad where a plain of stone used to emit up to 256.

**Where.** Entirely inside the mesher (`Mesher::TryStashGreedyQuad` /
`Mesher::FlushGreedyQuads`), per section, per layer, per face direction, per
16x16 plane. `AddBlockFace` routes each opaque/cutout quad through the
eligibility test; a passing quad parks in a thread-local per-plane grid
instead of being emitted, and after the section's block loop the flush runs a
classic 2D greedy scan (extend along u, then grow whole rows along v) per
(sprite atlas rect, packed corner color) key and emits one quad per rectangle.
Ineligible quads and 1x1 survivors are emitted through the ORIGINAL path,
verbatim.

**The pixel-identity contract.** Merging must not change the rendered image,
so the eligibility test errs toward "don't merge":

- full-cube element only (`from == (0,0,0)`, `to == (16,16,16)`), no element
  rotation, no per-block offset;
- face UV is the sprite's full tile (`uv == (0,0,16,16)`, `uvRotation == 0`);
- all four corner colors identical after the AO * tint * face-shade bake — a
  quad with any corner gradient (AO darkening, biome-blend edge) never merges;
- **translucent (almost) never merges**: the back-to-front re-sort orders per
  quad by centroid, and a merged quad is a coarser sorting unit that could
  blend in the wrong order against geometry in front of part of it. Fluid
  SIDE faces and non-flat surfaces never merge either (per-block heights and
  UV sub-rects). The one carve-out is flat still-fluid horizontal surfaces —
  see "Still-fluid greedy merging" below.

Merged corner positions reuse the exact integer block coordinates the
original outermost quads carried, so merged geometry never cracks or
T-junctions against unmerged neighbours.

**How one quad tiles N sprites.** An atlas cannot repeat a sub-rect with wrap
modes, so the terrain vertex grew from 24 to 32 bytes (`Render::TerrainVertex`,
`GetTerrainVertexLayout()`, `ChunkMegaBuffer::VERTEX_STRIDE = 32`):

| offset | bytes | field |
|---|---|---|
| 0 | 12 | pos (3x float32, world space) |
| 12 | 8 | uv (2x float32) — atlas UV, or TILE space when merged |
| 20 | 4 | color (RGBA8) |
| 24 | 4 | tileOrigin (2x unorm16) — sprite min corner in atlas UV |
| 28 | 4 | tileSize (2x unorm16) — sprite extent; **(0,0) = untiled flag** |

A merged quad's uv runs 0..N in block repeats; the terrain fragment shaders
compute `atlasUV = tileOrigin + fract(uv) * tileSize` and sample with
`textureGrad(atlas, atlasUV, dFdx(uv)*tileSize, dFdy(uv)*tileSize)` — the
UNWRAPPED uv's derivatives, so mip selection matches an unmerged quad and
fract()'s sawtooth cannot spike the LOD at block seams. `tileSize == 0` takes
the plain `texture()` path, keeping every unmerged quad bit-identical to the
old shaders. The tile rect is the FULL sprite rect from `AtlasBuilder`
(`GetUVRect`, no insets — same rect the per-block UV math uses), so edge
sampling behaviour is unchanged.

**Terrain-only format.** `GetBlockVertexLayout()` stays 24 bytes and every
entity/BE/GUI/portal renderer keeps it. The terrain shaders are their own
files (`shaders/terrain.vert`, `terrain_opaque.frag`, `terrain_cutout.frag`,
`terrain_solid.frag` + `_vk` twins) because `block.vert`/`block.frag` are
shared by entity renderers whose Vulkan pipelines use the 24-byte layout — a
shader consuming attribute 3 against a pipeline that doesn't provide it is
invalid in Vulkan. On Vulkan, `ChunkRenderer::Initialize` registers
`GetTerrainVertexLayout()` on the three terrain shaders; on OpenGL the shared
terrain VAO (`GLBackend::SetupBlockVertexFormat`) carries attribute 3
(GL_UNSIGNED_SHORT x4, normalized, offset 24).

**Measuring the win.** `PROFILE_PLOT("Mesh/QuadsMerged", ...)` plots the
quads each section did NOT emit thanks to merging;
`Mesher::MeshStats::quadsMerged` / `quadsMergedAway` carry the per-section
numbers. A/B: launch with `OBEY_NO_GREEDY=1` to disable merging at mesh time
(set at launch; the world load's remesh makes it take effect), same pattern
as `OBEY_SKIP`.

**Known deviations (deliberate, sub-pixel).**

- The tile rect is unorm16-quantized: at most 0.5/65535 of atlas UV error
  (~0.03 texel in a 4096px atlas), merged quads only.
- A fragment whose interpolated tile-uv lands EXACTLY on an integer block
  boundary resolves fract() to 0 instead of 1 (the standard greedy-tiling
  caveat); fragment centers land there with measure-zero probability.
- Quads with a non-zero `uvRotation` are never merged even against
  same-rotation neighbours — the tile math assumes unrotated orientation.
  Rotated-sprite terrain (some pillar/log variants) simply keeps per-block
  quads.

## Still-fluid greedy merging (2026-08-30)

The ocean extension of the pass above: flat water (and lava) surfaces now
merge too, which turns an ocean section's 256 top quads + 256 underside
copies into a handful of plates. Implemented entirely in `FluidMeshBuilder`
(`BeginGreedySection` / `TryStashGreedyFluidQuad` / `FlushGreedyFluidQuads`),
with its own thread-local grid, because fluid quads live on fractional Y
planes (the surface sits at `y + 0.899` inside cell y) and merge on
fluid-domain keys the solid grid has no slot for. The Mesher stays the
driver: `BuildSectionMeshFromCache` arms the fluid grid with the section base
and the resolved greedy switch (config flag AND `OBEY_NO_GREEDY`, so the two
passes cannot disagree), and `Mesher::FlushGreedyQuads` runs the fluid flush
first, folding its counts into the same stats.

**Eligibility (all must hold, checked geometrically per quad):**

- flat — all four corner heights equal. Deliberately a check on the actual
  vertices, not on the flow state: today even "flowing" tops are flat
  (per-corner fluid heights don't exist yet) and merge pixel-identically;
  the day real corner heights arrive they fail this check and drop back to
  per-block automatically;
- full-cell footprint with the canonical corner walk and the sprite's full
  rect in the canonical orientation (V along +Z) — one exact-float layout
  check (`IsCanonicalStillTopQuad` / `...BottomQuad`) enforces all three;
- all four corner colors identical (biome tint uniform — same rule as
  terrain; the 5x5 tint blend splits rectangles at biome borders);
- cells match on (surface height, sprite rect, packed color, output layer).

Three quad shapes participate, each in its own plane family so windings never
mix: the up-facing top surface, its reverse-wound `backwardUpFace` underside
copy, and the flat volume bottom (`CreateFluidBottomFace`). Side faces stay
per-block, always. Lava merges by the same rules into the opaque layer.

**Winding and the resort.** A merged plate encodes its facing in VERTEX
order, never index order — `TranslucentSort::BuildSortedIndices` rebuilds
every quad's six indices from the one forward template, so the plate survives
the re-sort exactly as the per-block backward-up face does. Verified corner
by corner against the single-quad emitters: a 1x1 survivor is re-emitted
verbatim (tile rect 0) and is bit-identical to the non-greedy build. The
tiled sprite is legal because `water_still`'s ATLAS RECT is constant — the
animation updates texels in place — and the translucent pass draws with
`terrain_solid.frag`/`_vk`, which carry the same `fract()` tiling path as the
other terrain shaders.

**Accepted risk (the documented deviation).** Sorting keys off one centroid
per quad, and a merged plate's centroid is coarser than the 1-block quads it
replaced. A flat ocean is safe — coplanar quads cannot occlude one another,
so their relative blend order never matters. The residual is STACKED
translucent along a ray through one plate: glass or a second water surface
above/below one corner of a large plate can blend in the wrong order right
there. Rare (fluid-over-translucent stacks only) and taken deliberately for
the ocean win.

**Debug/A-B.** The greedy debug view paints merged water plates on the same
`GreedyHeatColor(area)` scale as terrain, eligible-but-unmerged still tops
pure red, and every other fluid quad in the ineligible blue-gray. Water cells
and rects ride the existing since-launch eligible/rects totals, the
per-section `MeshStats::quadsMerged`/`quadsMergedAway`, and the
`Mesh/QuadsMerged` plot. A/B exactly as before: `OBEY_NO_GREEDY=1` at launch
disables both passes together.
