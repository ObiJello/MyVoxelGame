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
        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, float partialTick);

        // Same quarter-of-render-distance rule ItemEntityRenderer uses: the
        // server only sends these within its own tracking range, so drawing
        // further would promise more than the wire delivers.
        void SetRenderDistanceChunks(int chunks, float entityDistanceScaling = 1.0f);

    private:
        // The geometry for ONE block state. Called once per distinct state per
        // frame, not once per entity — a thousand primed TNT share one mesh.
        static void BuildStateMesh(Game::BlockState state,
                                   std::vector<ItemCubeVert>& verts,
                                   std::vector<uint32_t>& idx);

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
        struct Instance {
            glm::vec3 translate;
            float     scale;
        };
        static_assert(sizeof(Instance) == 16, "Instance is one vec4 attribute");

        // Everything the gather pass decides about one visible entity.
        struct DrawItem {
            Game::BlockState state;
            Instance         instance;
            bool             whiteFlash;
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
        std::vector<std::vector<DrawItem>> m_gatherParts;

        float m_cullRadius = 128.0f;
    };

    extern BlockCubeEntityRenderer g_blockCubeEntityRenderer;

} // namespace Render
