// File: src/client/renderer/entity/LeashRenderer.hpp
//
// Leads, as MC draws them:
//
//   * The ROPE — MC EntityRenderer.extractRenderState's LeashState (one per
//     lead, four for a quad connection) and LeashFeatureRenderer: a
//     24-segment ribbon from the leashed mob's leash offset to its holder's
//     rope-hold position, built as two crossed strips (so it reads from any
//     side), sagging (`slack`) unless it is a quad connection, alternating
//     brown shades per segment, and lit per vertex by the block and sky
//     light lerped between the two ends' eye cells.
//   * The KNOT — MC LeashKnotRenderer / LeashKnotModel: a 6x8x6 box on
//     lead_knot.png around the fence post, lit by its cell.
//
// The holder is resolved from the id the server linked (SetEntityLinkS2C,
// kept on the mob as its delayed holder id — MC getLeashHolder's client
// branch): a mob or knot from the mob manager, a remote player from the
// player list, or the local player. A holder the client does not know (out
// of tracking range, not sent yet) draws no rope, as in MC.
//
// Owned and called by MobRenderer, so every pass that draws mobs — the
// main view, portal views, panorama faces — draws their leads with the same
// frustum and clip plane. Own streaming buffers, per-frame double-buffered
// with a per-call cursor (EntityFrame.hpp), like every entity renderer.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct Frustum;
namespace Client { class ClientMobManager; }

namespace Render {

    class LeashRenderer {
    public:
        LeashRenderer() = default;
        ~LeashRenderer();

        bool Initialize();
        void Shutdown();

        // The local camera, from the frame loop once per frame before the
        // entity passes: whether the player's own body is on screen (third
        // person, free camera) — the lead then ends in the model's hand —
        // or not (first person: it ends where the hand is on screen, MC
        // LocalPlayer.getRopeHoldPosition), and the eye and look it uses.
        static void SetLocalView(bool bodyVisible, const glm::dvec3& eye, float yawDeg, float pitchDeg);

        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    const Client::ClientMobManager& mobs, float partialTick);

        // MC EntityRenderState.LeashState.
        struct LeashState {
            glm::dvec3 start{0.0};     // world: the leashed mob's attachment
            glm::dvec3 end{0.0};       // world: the holder's
            int  startBlockLight = 0;
            int  endBlockLight   = 0;
            int  startSkyLight   = 15;
            int  endSkyLight     = 15;
            bool slack = true;
        };

    private:
        // What the rope needs of its holder (MC getRopeHoldPosition, the eye
        // cell for the light, and the quad-holder offsets).
        struct Holder {
            glm::dvec3 position{0.0};      // interpolated feet
            glm::dvec3 ropeHold{0.0};
            glm::dvec3 eye{0.0};
            float      bodyYawDeg = 0.0f;
            bool       onFire = false;
            bool       quadHolder = false;
            std::array<glm::dvec3, 4> quadOffsets{};
        };
        bool ResolveHolder(int32_t id, const Client::ClientMobManager& mobs, float partialTick,
                           Holder& out) const;

        // LeashFeatureRenderer.prepare: one LeashState's two strips, as
        // indexed triangles in render space.
        void AppendLeash(const LeashState& state);
        // LeashKnotRenderer.submit for one knot.
        void AppendKnot(const glm::dvec3& feet, const glm::vec3& light);

        TextureHandle KnotTexture();

        ShaderHandle m_shader = INVALID_SHADER;

        struct FrameBuffers {
            BufferHandle vb   = INVALID_BUFFER;
            BufferHandle ib   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
        };
        FrameBuffers m_frames[EntityFrame::kMaxSlots];   // EntityFrame::Slots() of them exist
        EntityFrame::Cursor m_frameCursor;
        size_t m_vertCursor = 0;
        size_t m_idxCursor  = 0;
        // Per set. A rope is 100 vertices (four ropes for a quad lead), a
        // knot 24 — room for a few hundred of each in view.
        static constexpr size_t kMaxVertices = 65536;
        static constexpr size_t kMaxIndices  = 196608;

        TextureHandle m_whiteTexture = INVALID_TEXTURE;
        TextureHandle m_knotTexture  = INVALID_TEXTURE;
        bool          m_knotTextureTried = false;
        int           m_textureGeneration = -1;   // Resources::CacheStale

        // MC LeashKnotModel: root → "knot", one 6x8x6 cube at texOffs 0,0 on
        // a 32x32 sheet.
        std::unique_ptr<ModelPart> m_knotModel;

        std::vector<ModelVertex> m_verts;
        std::vector<uint32_t>    m_indices;

        bool m_initialized = false;
    };

} // namespace Render
