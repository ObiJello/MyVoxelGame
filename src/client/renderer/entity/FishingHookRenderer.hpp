// File: src/client/renderer/entity/FishingHookRenderer.hpp
//
// MC FishingHookRenderer: the bobber and its line.
//
//   * The BOBBER — a camera-facing 0.5 x 0.5 quad of
//     textures/entity/fishing/fishing_hook.png centred on the hook
//     (entityCutoutCull), lit by the light at the hook.
//   * The LINE — sixteen segments from the hook (0.25 above it) to the
//     fisher's hand, sagging along MC's curve y = dy * (a² + a) / 2, black and
//     unlit (RenderTypes.lines). MC draws it as screen-space lines of
//     Window.getAppropriateLineWidth = max(2.5, width / 1920 * 2.5) pixels;
//     here it is a camera-facing ribbon whose world width is recomputed per
//     vertex from its view depth, which puts the same pixel width on screen.
//
// Where the line starts (MC getPlayerHandPos):
//   * the local player in first person: MC's point on the camera's near plane
//     (0.525 of the half-width to the holding side, 0.1 of the half-height
//     down, scaled by 960 / fov) turned by the arm swing — the tip of the rod
//     the first-person hand holds;
//   * everyone else (and the local player's own body in third person): the
//     ENGINE's player model — the stick figure's hand joint on the holding
//     arm (StickFigureHand, the joint its arm line ends at), scaled with the
//     figure; a morphed player's line leaves 0.7 of the way up its body, as
//     the lead does. DELIBERATE DIVERGENCE: MC's third-person offsets are for
//     its own model, whose hand this figure does not have.
// The holding arm is the main hand when it holds the rod, else the off hand
// (FishingHookRenderer.getHoldingArm), synced with the hook.
//
// Owned and called by MobRenderer next to the leads, so every pass that draws
// mobs draws the lines with the same frustum and clip plane. Own streaming
// buffers, double-buffered per frame with a per-call cursor (EntityFrame.hpp).
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "client/renderer/entity/EntityFrame.hpp"
#include "client/renderer/entity/model/ModelPart.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct Frustum;
namespace Client { class ClientMobManager; }
namespace Game { class FishingHook; }

namespace Render {

    class FishingHookRenderer {
    public:
        FishingHookRenderer() = default;
        ~FishingHookRenderer();

        bool Initialize();
        void Shutdown();

        // The local camera, once per frame from the frame loop: whether the
        // player's own body is on screen (third person, free camera) and the
        // eye, look and FOV MC's first-person line origin is built from.
        static void SetLocalView(bool bodyVisible, const glm::dvec3& eye, float yawDeg, float pitchDeg,
                                 float fovDeg);
        // The framebuffer the frame draws to — the line's pixel width and the
        // near plane's aspect.
        static void SetFramebufferSize(int width, int height);

        void Render(const glm::mat4& projection, const glm::mat4& view,
                    const glm::vec3& cameraPos, const Frustum& frustum,
                    const Client::ClientMobManager& mobs, float partialTick);

    private:
        // MC getPlayerHandPos for the hook's owner; false when the owner is
        // not known here (then, as in MC, nothing is drawn).
        bool ResolveHandPos(const Game::FishingHook& hook, float partialTick, glm::dvec3& out) const;

        void AppendBobber(const glm::dvec3& pos, const glm::vec3& right, const glm::vec3& up,
                          const glm::vec3& light);
        void AppendLine(const glm::dvec3& hookPos, const glm::dvec3& handPos, const glm::dvec3& camera,
                        const glm::mat4& view, float pixelsPerUnitAtDepth1, float widthPx);

        TextureHandle HookTexture();

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
        // Per set: a hook is 4 bobber + 34 line vertices.
        static constexpr size_t kMaxVertices = 16384;
        static constexpr size_t kMaxIndices  = 49152;

        TextureHandle m_whiteTexture = INVALID_TEXTURE;
        TextureHandle m_hookTexture  = INVALID_TEXTURE;
        bool          m_hookTextureTried = false;
        int           m_textureGeneration = -1;   // Resources::CacheStale

        std::vector<ModelVertex> m_verts;
        std::vector<uint32_t>    m_indices;

        bool m_initialized = false;
    };

} // namespace Render
