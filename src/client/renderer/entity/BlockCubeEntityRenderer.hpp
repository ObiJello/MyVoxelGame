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
        // `aoCell` is the cell whose neighbourhood shades the block — MC's
        // MovingBlockRenderState.blockPos, the cell the block is moving out
        // of — so a carried block keeps the ambient occlusion it had.
        void SubmitMovingBlock(Game::BlockState state, const glm::dvec3& worldMin,
                               const glm::ivec3& aoCell);

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
        };
        FrameBuffers m_cubeFrames[2];
        EntityFrame::Cursor m_frameCursor;
        size_t m_vertCursor = 0;
        size_t m_idxCursor  = 0;

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
        // backend without instancing (Vulkan today), and Render falls back to
        // the per-entity loop below. Both paths stay live; the instanced one is
        // an accelerator, not a replacement.
        ShaderHandle  m_instShader = INVALID_SHADER;
        BufferHandle  m_instanceVB = INVALID_BUFFER;

        // One instance on the wire: world translation and a uniform scale.
        // Both block entities' model transforms collapse to exactly that —
        // MC's FallingBlockRenderer is a pure translate, and TntRenderer's
        // lift/swell/centre chain is a translate of a scale about the cube's
        // centre — so a mat4 per instance was 64 bytes carrying 16 bytes of
        // information, and at a hundred thousand falling blocks that is the
        // difference between 6 MB and 1.6 MB uploaded per frame.
        //
        // `light` rides beside it (location 4): during the gather the
        // entity's MC packed light (getPackedLightCoords at its eye — the
        // gather may run on workers, which only read the level), turned into
        // the lightmap colour, RGBA8, on the main thread before the upload.
        struct Instance {
            glm::vec3 translate;
            float     scale;
            uint32_t  light = 0xFFFFFFFFu;
        };
        static_assert(sizeof(Instance) == 20, "Instance is a vec4 + an RGBA8 light");

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
        };

        // Instances the buffer holds across ALL groups in a frame. A group is
        // one block state at one flash state, so a pure sand pyramid is one
        // group however many entities it holds — and the cap is per FRAME,
        // because each group is written at its own offset. Past it the
        // remainder is dropped for the frame. Was 65,536, which silently
        // drew 65k of a 176k-block sand pyramid; 4 MB of instance buffer now.
        // 1.5M instances = 24 MB of instance buffer: a 200-base pyramid
        // (1.37M) fits in one frame.
        static constexpr size_t kMaxInstances = 1572864;

        // Gather scratch, kept for capacity: the per-frame item list and the
        // per-worker partial lists the parallel gather merges in order.
        std::vector<DrawItem>              m_items;
        struct MovingBlock { Game::BlockState state; glm::dvec3 worldMin; glm::ivec3 aoCell; };
        std::vector<MovingBlock>           m_movingBlocks;
        struct CustomMesh { std::vector<ItemCubeVert> verts; std::vector<uint32_t> idx; };
        std::vector<CustomMesh>            m_customMeshes;

        // Bake the terrain mesher's four-corner ambient occlusion into a
        // block-model mesh, sampling the world around `aoCell`.
        static void ApplyAmbientOcclusion(Game::BlockState state, const glm::ivec3& aoCell,
                                          std::vector<ItemCubeVert>& verts,
                                          const std::vector<uint32_t>& idx);
        std::vector<std::vector<DrawItem>> m_gatherParts;

        float m_cullRadius = 128.0f;
    };

    extern BlockCubeEntityRenderer g_blockCubeEntityRenderer;

} // namespace Render
