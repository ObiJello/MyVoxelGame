// File: src/client/renderer/blockentity/SkyBlockRenderer.hpp
//
// Draws BlockID::SkyBlock — an engine block whose faces are a window onto
// the sky: whatever sky the view is showing (the vanilla sky with its sun,
// moon, stars and sunrise fan, a user / OptiFine skybox, the End's sky, the
// Nether's fog), seen through the block in SCREEN space, the way MC's end
// portal shows its starfield (TheEndPortalRenderer) rather than a texture on
// the cube.
//
// ── How ─────────────────────────────────────────────────────────────────
// Before the first terrain pass this renderer draws the visible sky-block
// faces DEPTH-ONLY (colour writes off, depth test and write on, back faces
// culled). The depth they write keeps everything behind them — terrain,
// entities, water, clouds, rain — from drawing over them; anything in front
// draws over them as usual. What shows in the hole is the sky pass's own
// output — no copy of the frame, no shader that has to know about skyboxes —
// so it is right for every sky the engine has (sun, moon, stars, a user /
// OptiFine skybox with its time-of-day rotation and fades) in every
// dimension:
//   - A view that draws its sky first (portal far sides, gun portals,
//     panorama faces, the main view under OBEY_LATE_SKY=0) already has the
//     sky in the colour buffer under the faces.
//   - The main view draws its sky late, depth-tested at the far plane, which
//     the faces' depth would keep out. Right after the faces ChunkRenderer
//     calls its sky-windows hook, and the sky is drawn into exactly the
//     face pixels (SkyRenderer::DepthMode::Windows: Greater against the far
//     plane, no depth write) before any terrain; the late pass fills the
//     rest of the frame afterwards.
//
// It is called from ChunkRenderer::RenderAll (after PrepareVisibleSections,
// before the opaque pass) so it runs for EVERY view the chunk renderer draws
// — the main view, each immersive-portal far view (inside its stencil mask,
// with the far side's sky behind it and the portal clip plane applied), the
// gun portals' views and the leave capture's panorama faces — and only for
// the sections that view actually draws.
//
// ── Why not the chunk mesh ──────────────────────────────────────────────
// The faces must be in the depth buffer before ANY terrain draws: inside the
// opaque pass a terrain fragment behind a sky face could already be in the
// colour buffer when the face arrives. So the mesher skips the block
// (Mesher::ProcessBlock) — its model is still a full cube, so it occludes
// and culls its neighbours like any opaque block — and this class finds the
// blocks through ClientChunkManager's per-chunk index (ClientChunk::
// skyBlocks / SkyBlockChunks), the EndPortalRenderer arrangement. A world
// without sky blocks costs one empty() check per view.
//
// ── Doors' sky faces ────────────────────────────────────────────────────
// A door's faces can be opened onto the sky too (Game::SkyFaces — a sky
// block used on the face with the sneak and sprint keys down). Those are
// the door model's own faces, named by the door's SkyFacesBlockEntity
// mask: the mesher leaves them out (Mesher::SetSkyFaces, published by
// ClientChunkManager) and this pass draws them depth-only with the sky
// blocks' faces, from ClientChunkManager::SkyFaces and the door's live
// state, so they follow the door as it swings.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "../entity/EntityFrame.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Render {

    class ChunkRenderer;

    class SkyBlockRenderer {
    public:
        ~SkyBlockRenderer();

        // Creates the shader and the streaming vertex buffers. False (having
        // logged) on failure; RenderWindows then does nothing and sky blocks
        // show whatever terrain lies behind them.
        bool Initialize();
        void Shutdown();

        // One view's sky-block faces, depth-only, with the view's terrain
        // MVP and portal clip plane (render space). `sections` answers which
        // sections this view draws. True when something was drawn — the
        // caller's shader, textures and vertex array are no longer bound.
        bool RenderWindows(const ChunkRenderer& sections, const glm::mat4& mvp,
                           const glm::vec4& portalClipPlane);

    private:
        // The shared 24-byte block vertex layout (GetBlockVertexLayout); the
        // shader reads only the position.
        struct Vert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vert) == 24, "vertex stride must match block layout");

        bool m_initialized = false;
        ShaderHandle m_shader = INVALID_SHADER;
        // 1×1, never sampled: Vulkan's block-style pipelines skip a draw
        // with no texture bound on slot 0 (VKBackend::PrepareDraw), and the
        // pass must not depend on whatever the previous renderer left there.
        TextureHandle m_dummyTexture = INVALID_TEXTURE;

        // Two streaming sets alternated per frame (EntityFrame): Vulkan's
        // UpdateBuffer is a host memcpy and the previous frame may still be
        // drawing from the other set. Every view of a frame APPENDS at the
        // set's cursor — the portal views run after the main view, and
        // rewriting offset 0 would change what the main view's draw reads.
        struct FrameBuffers {
            BufferHandle vb = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t       capacityVerts = 0;
            size_t       usedVerts = 0;   // this frame's cursor
        };
        FrameBuffers m_frames[EntityFrame::kMaxSlots];   // EntityFrame::Slots() of them exist
        EntityFrame::Cursor m_frameCursor;

        std::vector<Vert> m_verts;   // reused: a steady frame allocates nothing
    };

    extern SkyBlockRenderer g_skyBlockRenderer;

} // namespace Render
