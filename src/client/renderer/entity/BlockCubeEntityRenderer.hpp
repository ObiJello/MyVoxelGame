// File: src/client/renderer/entity/BlockCubeEntityRenderer.hpp
//
// Draws the two entities that ARE a block: a falling sand/gravel/anvil block,
// and primed TNT.
//
// One renderer for both because they share everything expensive — the block
// shader, the streaming vertex buffer, the atlas binding, the fog environment —
// and differ only in their model matrix. MobRenderer cannot do this job: it is
// built around ModelPart skeletons, and a block has no skeleton. This is the
// ItemEntityRenderer pattern instead, and it uses the same
// Render::BuildBlockModelMesh, so a falling cobblestone and a dropped
// cobblestone are the same geometry.
//
// ── MC's transforms ──────────────────────────────────────────────────────
//
// FallingBlockRenderer:   translate(-0.5, 0, -0.5), draw. No rotation.
// TntRenderer:            translate(0, 0.5, 0); if fuse < 10, scale by
//                         1 + g^4 * 0.3 where g = 1 - fuse/10; then centre and
//                         draw, flashing white on alternating 5-tick windows.
//
// The quartic on the swell is what makes TNT look calm until the last half
// second and then lurch — a linear ramp reads as a slow inflate and is
// noticeably wrong.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../viewmodel/ItemMeshBuilder.hpp"   // ItemCubeVert
#include "EntityFrame.hpp"
#include "common/world/block/BlockState.hpp"

#include <glm/glm.hpp>
#include <vector>

namespace Render {

    class BlockCubeEntityRenderer {
    public:
        bool Initialize();
        void Shutdown();

        // Draw every falling block and primed TNT the client knows about.
        // `partialTick` (0..1) blends each entity's previous-tick position with
        // its current one — without it a falling block steps once per tick,
        // which at 0.04 gravity is very visible.
        // `movingBlocksOnly` draws just the blocks handed in through
        // SubmitMovingBlock since the last such call — the pass that runs
        // right after the block-entity renderers, in the same view. The
        // ordinary call (falling blocks, primed TNT) never consumes them, so
        // a portal or far view rendered in between cannot eat the main
        // view's moving pistons.
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick,
                    bool movingBlocksOnly = false);

        // Same quarter-of-render-distance rule ItemEntityRenderer uses: the
        // server only sends these within its own tracking range, so drawing
        // further would promise more than the wire delivers.
        void SetRenderDistanceChunks(int chunks, float entityDistanceScaling = 1.0f);

        // The geometry for ONE block state, in the unit block cell with atlas
        // UVs. Called once per distinct state per frame here, not once per
        // entity — a thousand primed TNT share one mesh. Public for the mob
        // renderer: a sulfur cube draws its swallowed block through this
        // under its own pose (MC SulfurCubeInnerLayer's block submit).
        static void BuildStateMesh(Game::BlockState state,
                                   std::vector<ItemCubeVert>& verts,
                                   std::vector<uint32_t>& idx);

        // A block model drawn at an arbitrary world position this frame —
        // MC SubmitNodeCollector.submitMovingBlock, which the piston renderer
        // uses for the block a piston is carrying. `worldMin` is the model's
        // origin corner in world space. Consumed by the next Render call.
        // `lightCell` is the cell whose light and neighbourhood light the
        // block — MC's MovingBlockRenderState.blockPos (the cell a carried
        // block is leaving, the base's own cell, the head's cell) — lit per
        // vertex exactly as the section mesher would light the block resting
        // there, so the hand-over between the two never changes its look.
        // It is also where the block's tint is read (MC: the state's biome
        // is level.getBiome(blockPos), resolved at blockPos):
        // `restingStandIn` false — a block in flight — takes MC
        // MovingBlockRenderState.getBlockTint's single, unblended biome;
        // true — a block standing in for the section mesh's block in
        // `lightCell` (landed, run its course, a retracting piston's base) —
        // takes the section mesher's blended tint there, for the same
        // no-step hand-over as its light. Face shade is the bound level's
        // cardinal lighting (MC MovingBlockRenderState.cardinalLighting).
        void SubmitMovingBlock(Game::BlockState state, const glm::dvec3& worldMin,
                               const glm::ivec3& lightCell, bool restingStandIn);

    private:

        bool m_initialized = false;

        ShaderHandle  m_shader = INVALID_SHADER;

        // The per-frame state-mesh geometry: two streaming sets alternated
        // per FRAME, each call in a frame (main pass, portal recursions)
        // appending at a cursor — see EntityFrame.hpp. `instMesh` is the
        // instanced VAO over the SAME vertex/index buffers plus the shared
        // instance buffer (INVALID where the backend has no instancing).
        struct FrameBuffers {
            BufferHandle vb       = INVALID_BUFFER;
            BufferHandle ib       = INVALID_BUFFER;
            MeshHandle   mesh     = INVALID_MESH;
            MeshHandle   instMesh = INVALID_MESH;
            // Per slot like vb/ib: frames overlap on the GPU (CLAUDE.md), so
            // a frame must never write the instances the previous frame's
            // draws still read.
            BufferHandle inst     = INVALID_BUFFER;
            size_t       instCapacity = 0;   // instances `inst` holds
            // Shrink bookkeeping: the most this slot's current frame wrote,
            // the peak over the current low-use stretch, and when that
            // stretch began (seconds, steady clock).
            size_t       instFrameUsed = 0;
            size_t       instLowPeak   = 0;
            double       instLowSince  = 0.0;
        };
        // Replaces `fb`'s instance buffer + instanced mesh with a `capacity`
        // pair; the old pair is deferred-destroyed. False = kept the old one.
        bool ResizeInstanceBuffer(FrameBuffers& fb, size_t capacity);
        // Called when `fb` starts a new frame: shrinks a grown buffer once it
        // has stayed at or under a quarter full for kInstanceShrinkSeconds.
        void MaybeShrinkInstanceBuffer(FrameBuffers& fb);
        // Grows `fb`'s instance buffer (and its instanced mesh, which names
        // the buffer) to hold at least `needed` instances, doubling up to
        // kMaxInstances. The old pair is deferred-destroyed, so draws this
        // frame already recorded against it stay valid; a grow therefore
        // restarts m_instCursor at 0 in the new buffer. False = could not
        // grow (the old buffer is kept).
        bool EnsureInstanceCapacity(FrameBuffers& fb, size_t needed);
        VertexLayout m_instanceLayout;
        FrameBuffers m_cubeFrames[EntityFrame::kMaxSlots];   // EntityFrame::Slots() of them exist
        EntityFrame::Cursor m_frameCursor;
        size_t m_vertCursor = 0;
        size_t m_idxCursor  = 0;
        // The instance buffer's frame cursor, like the vertex one: every
        // call in a frame (the main pass, the moving-block pass, portal
        // views) writes its instances past the previous call's. Vulkan
        // reads the instance buffer when the frame executes, so a second
        // call writing from zero again drew the first call's groups with
        // the second call's instances.
        size_t m_instCursor = 0;

        // ── The instanced path ─────────────────────────────────────────────
        //
        // Same vertex and index buffers, a second VAO that additionally reads a
        // per-instance model matrix, and a shader that applies it. One
        // glDrawElementsInstanced per (block state, flash state) group instead
        // of one glDrawElements per entity — which at Apple's ~1 us/sub-draw
        // floor was 21 ms of frame time for twenty thousand TNT, 38.6% of the
        // detonation window and the client's largest single cost.
        //
        // NULLABLE BY DESIGN. CreateInstancedMesh answers INVALID_MESH on a
        // backend without instancing, and Render falls back to the
        // per-entity loop below (one light per draw: the light cell's).
        // Both paths stay live; the instanced one is an accelerator, not a
        // replacement.
        ShaderHandle  m_instShader = INVALID_SHADER;

        // One instance on the wire: world translation and a uniform scale.
        // Both block entities' model transforms collapse to exactly that —
        // MC's FallingBlockRenderer is a pure translate, and TntRenderer's
        // lift/swell/centre chain is a translate of a scale about the cube's
        // centre — so a mat4 per instance was 64 bytes carrying 16 bytes of
        // information.
        //
        // `cells` (locations 4-7) is the instance's light: the MC packed
        // light (block | sky << 4) of the 27 cells around its light cell,
        // index CellIndex(dx, dy, dz), which block_instanced.vert blends per
        // vertex by the recipe the mesh carries in its vertex alpha. A
        // falling block fills all 27 (MC MovingBlockRenderState.blockPos and
        // its neighbourhood, see FillFallingLight); primed TNT and a moving
        // block without a lit mesh fill only the own cell, which every
        // vertex of their meshes reads. Byte 27 is flags (kCellFlagPreLit).
        struct Instance {
            glm::vec3 translate;
            float     scale;
            uint8_t   cells[28] = {};
        };
        static_assert(sizeof(Instance) == 44, "Instance is a vec4 + 27 cell lights + flags");
        static constexpr int     kOwnCell        = 13;   // CellIndex(0, 0, 0)
        static constexpr int     kCellFlags      = 27;
        static constexpr uint8_t kCellFlagPreLit = 1;    // the vertex colours carry the light
        static constexpr int CellIndex(int dx, int dy, int dz) { return (dx + 1) * 9 + (dy + 1) * 3 + (dz + 1); }

        // The falling-block light cache, slot-for-slot with the gather's
        // proxy index. An entry is a pure function of its key (the light
        // cell, the carried state — its emission — and the level's light
        // version), so it is right for whichever entity sits at that index
        // now; a mismatch just recomputes. MC reads the light every frame;
        // this reads it again only when something it depends on moved.
        struct LightCacheEntry {
            glm::ivec3 cell{0};
            uint32_t   stateRaw = 0;
            uint32_t   lightVersion = ~0u;   // the low word; it never wraps in a session
            uint8_t    cells[27] = {};
        };
        static_assert(sizeof(LightCacheEntry) == 48, "48 bytes a falling block");
        std::vector<LightCacheEntry> m_lightCache;
        const void*                  m_lightCacheLevel = nullptr;

        // Everything the gather pass decides about one visible entity.
        struct DrawItem {
            Game::BlockState state;
            Instance         instance;
            bool             whiteFlash;
            // Which mesh draws it: the state's shared mesh (meshKey ==
            // state.RawId(), customMesh < 0) or one of this pass's per-block
            // meshes with ambient occlusion baked in (moving blocks).
            uint32_t         meshKey    = 0;
            int              customMesh = -1;
            // The custom mesh carries its light per vertex (moving blocks):
            // the instance flags kCellFlagPreLit.
            bool             preLit     = false;
            // A falling block: drawn the way MC's MovingBlockFeatureRenderer
            // draws it — the level's face shade and the block's in-world
            // tint — where primed TNT keeps the entity block-model look.
            bool             falling    = false;
            // Its tint reads a biome (BlockTint Source::Biome / FlowerBed):
            // MC FallingBlockRenderer's blockPos, containing(x, maxY, z),
            // where the biome is read on the main thread after the gather.
            bool             biomeTint  = false;
            glm::ivec3       tintCell{0};
        };

        // Instances the buffer holds across ALL groups in a frame. A group is
        // one block state at one flash state, so a pure sand pyramid is one
        // group however many entities it holds — and the cap is per FRAME,
        // because each group is written at its own offset. Past it the
        // remainder is dropped for the frame. Was 65,536, which silently
        // drew 65k of a 176k-block sand pyramid; 4 MB of instance buffer now.
        // 1.5M instances = 66 MB of instance buffer (44 bytes each, the
        // light cells included): a 200-base pyramid (1.37M) fits in one
        // frame.
        static constexpr size_t kMaxInstances = 1572864;
        // Where each slot's instance buffer starts (~0.7 MB); it doubles on
        // demand toward kMaxInstances, so ordinary play never pays for the
        // pyramid-sized cap.
        static constexpr size_t kInitialInstances = 16384;
        // How long a grown buffer must stay at or under a quarter full before
        // it is shrunk back (to twice that stretch's peak, at least the
        // initial size). Long enough that a TNT burst followed by a lull
        // doesn't bounce between grow and shrink.
        static constexpr double kInstanceShrinkSeconds = 60.0;

        // Gather scratch, kept for capacity: the per-frame item list and the
        // per-worker partial lists the parallel gather merges in order.
        std::vector<DrawItem>              m_items;
        struct MovingBlock { Game::BlockState state; glm::dvec3 worldMin; glm::ivec3 lightCell; bool restingStandIn; };
        std::vector<MovingBlock>           m_movingBlocks;
        struct CustomMesh { std::vector<ItemCubeVert> verts; std::vector<uint32_t> idx; };
        std::vector<CustomMesh>            m_customMeshes;

        // Bake the section mesher's per-vertex light and ambient occlusion
        // (BlockModelLighter), the block's in-world tint (BlockTint) and the
        // bound level's face shade into a block-model mesh, sampling the
        // live client level around `cell`. `quads` is the builder's per-quad
        // record (BuildBlockModelMeshFrom); without it (the cube fallback)
        // only light and AO are applied.
        static void LightMovingBlock(Game::BlockState state, const glm::ivec3& cell, bool restingStandIn,
                                     std::vector<ItemCubeVert>& verts,
                                     const std::vector<uint32_t>& idx,
                                     const std::vector<BlockModelQuad>& quads);

        // A falling block's mesh, as MC MovingBlockFeatureRenderer
        // tesselates it over the MovingBlockRenderState: the state's model
        // with each quad's in-world tint (`biomeColor` the packed biome
        // colour for a biome-tinted state, else unused), the bound level's
        // face shade and — when the model is lit smooth — the render
        // state's ambient occlusion (its neighbours are all AIR, so the AO
        // is the model's alone) written into the vertex colours. With
        // `lightRecipes` each vertex's alpha is its light recipe for
        // block_instanced.vert (which cells of the instance's 3x3x3 its
        // light blends); without (the per-entity fallback, which lights a
        // draw with one colour) the alpha stays opaque.
        static void BuildFallingMesh(Game::BlockState state, uint32_t biomeColor, bool lightRecipes,
                                     std::vector<ItemCubeVert>& verts,
                                     std::vector<uint32_t>& idx);

        // A falling block's instance light: MC's light coords —
        // LightCoordsUtil.getLightCoords(state, MovingBlockRenderState, pos),
        // the level's light with the carried state's emission — of the 27
        // cells around `cell` (MC MovingBlockRenderState.blockPos).
        static void FillFallingLight(Game::BlockState state, const glm::ivec3& cell, uint8_t (&out)[27]);

        // The state's model with the builder's per-quad record (`quads` left
        // empty when the cube fallback had to draw it).
        static void BuildStateMesh(Game::BlockState state,
                                   std::vector<ItemCubeVert>& verts,
                                   std::vector<uint32_t>& idx,
                                   std::vector<BlockModelQuad>& quads);
        std::vector<std::vector<DrawItem>> m_gatherParts;

        float m_cullRadius = 128.0f;
    };

    extern BlockCubeEntityRenderer g_blockCubeEntityRenderer;

} // namespace Render
