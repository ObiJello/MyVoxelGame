// File: src/client/renderer/portal/ImmersivePortalRenderer.hpp
//
// See-through rendering of immersive portals — the Immersive Portals mod's
// RendererUsingStencil, on this engine's render backend.
//
// HOW A PORTAL IS DRAWN (one "layer" = one level of nesting; stencil = layer)
//   1. Mark: draw the portal's surface with stencil EQUAL layer → INCR, depth
//      tested, no colour or depth written. Pixels where the surface is
//      visible now hold layer+1.
//   2. Clear depth inside the mark: draw the surface again through a
//      projection that puts every vertex at the far plane, depth ALWAYS and
//      written, colour = the far dimension's fog. This is the mod's
//      glDepthRange(1,1) trick expressed as a matrix, so it needs no special
//      shader and works identically on GL and Vulkan.
//   3. Render the far side: the viewer's camera pushed through the portal's
//      transform, the far level bound (ClientLevels::WithLevel), a clip plane
//      at the destination surface so nothing behind it is drawn, the
//      stencil override set to EQUAL layer+1 so the far world only lands
//      inside the mark, and the far dimension's fog and sky. The portals OF
//      that level are then drawn the same way at layer+1, up to kMaxLayers.
//   4. Restore depth: draw the surface with depth ALWAYS so what is behind
//      the portal in THIS world is occluded by it from now on.
//   5. Clamp stencil: every pixel holding more than `layer` is set back to
//      `layer`, so the next portal at this layer starts from a clean mask.
//
// No oblique near plane (the gun renderer's approach): the mod never needed
// one — the clip plane does the culling and depth clamp on the surface keeps
// it drawable when the camera stands in it — and it is what the Vulkan
// backend's Z remap cannot support anyway.
//
// The world itself is drawn by a callback supplied by the frame loop
// (LevelRenderFn): sky, terrain, entities, block entities for the bound
// level, from the given camera. The renderer only manages masks, depth,
// transforms and recursion.
#pragma once

#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "../backend/RenderTypes.hpp"
#include "../core/Camera.hpp"
#include "../core/Frustum.hpp"
#include "common/portal/ImmersivePortal.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Render {

    class ImmersivePortalRenderer {
    public:
        // Nesting depth. The mod's maxPortalLayer default; the 8-bit stencil
        // buffer allows 255, the frame budget does not.
        static constexpr int kMaxLayers = 5;

        // Everything the frame loop needs to draw one level from one
        // viewpoint. The level is already BOUND (globals point at it).
        struct ViewContext {
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            Camera            camera;        // eye on the far side; view via viewOverride
            glm::mat4         view{1.0f};
            glm::mat4         projection{1.0f};
            Frustum           frustum{};     // bounded by the portal's edges
            float             partialTick = 0.0f;
            int               layer = 1;     // 1 = seen through one portal
            // The portal this view looks through (in the level one layer up).
            const Game::Immersive::Portal* through = nullptr;
        };
        using LevelRenderFn = std::function<void(const ViewContext&)>;

        bool Initialize();
        void Shutdown();

        // Draw every visible immersive portal of the BOUND level, recursively.
        // Call after the level's own opaque/entity passes so the marks depth-
        // test against them. `aspect` matches the main projection;
        // `renderDistanceChunks` is the effective render distance (the
        // client's setting clamped by the server), from which the mod's
        // portal range and far-view distance rules are derived.
        // `renderLevel` draws the bound level for a far view. `renderCrossers`
        // draws the bound level's clip-capable entities (items, orbs, block
        // cubes) for the CROSSING pass: for every visible portal, the far
        // level's entities are drawn into the near world through the
        // portal's inverse transform, clipped to what lies in front of the
        // surface — the half of an item that pokes out of a portal into
        // this side. Runs before the surfaces are marked, at every layer.
        void Render(const glm::mat4& projection, const glm::mat4& view, const Camera& camera,
                    const Frustum& frustum, float aspect, int renderDistanceChunks, float partialTick,
                    const LevelRenderFn& renderLevel, const LevelRenderFn& renderCrossers);

        // Draw a wireframe of every portal of the bound level (debugging;
        // OBEY_PORTAL_OUTLINES=1 or the Render Controls toggle).
        bool outlinesEnabled = false;

        // Portals drawn last frame, all layers — for the F3 readout.
        int PortalsRenderedLastFrame() const { return m_renderedLastFrame; }
        // Did this portal get a far view (top level) last frame? The
        // entity cut passes in PlatformMain (a body half-way through a
        // surface, drawn clipped on each side) cost an entity pass per
        // portal; a portal that was culled, too small, or over budget shows
        // no far side, so nothing cut at it can be seen either.
        bool DrewLastFrame(Game::Immersive::PortalId id) const {
            return m_drawnLastFrame.find(id) != m_drawnLastFrame.end();
        }

    private:
        struct SurfaceMesh {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            uint32_t     indexCount = 0;
            uint64_t     shapeKey = 0;     // what the mesh was built from
            uint64_t     lastUsedFrame = 0;
        };

        // The renderer's own frame counter (for mesh eviction).
        uint64_t m_frame = 0;
        int      m_renderedLastFrame = 0;
        int      m_renderedThisFrame = 0;
        bool     m_initialized = false;

        ShaderHandle  m_shader = INVALID_SHADER;
        TextureHandle m_dummyTexture = INVALID_TEXTURE;
        std::unordered_map<Game::Immersive::PortalId, SurfaceMesh> m_meshes;

        SurfaceMesh& MeshFor(const Game::Immersive::Portal& portal);
        void DestroyMesh(SurfaceMesh& m);
        void EvictUnusedMeshes();

        // One layer: every visible portal of the bound level from `camera`.
        struct Candidate {
            const Game::Immersive::Portal* portal;
            double distance;
        };
        // The portals of the BOUND level worth drawing from this camera,
        // nearest first — the mod's PortalRenderer.shouldSkipRenderingPortal:
        // visible, camera in front, within RenderRange, frustum-tested past
        // 0.1 blocks, not the reverse of the portal looked through
        // (cannotRenderInMe), not the portal two layers up when it is the
        // reverse of the one looked through (isInvalidRecursionRendering).
        // `through` is the portal this view looks through, `outerThrough`
        // the one the layer above looks through.
        std::vector<Candidate> Candidates(int layer, const Game::Immersive::Portal* through,
                                          const Game::Immersive::Portal* outerThrough,
                                          const glm::dvec3& eye, const Frustum& frustum,
                                          int renderDistanceChunks) const;

        // PortalRenderer.getRenderRange: how far a portal is drawn at all,
        // in blocks. The render distance, or 16 when reduced portal
        // rendering is set; divided by the layer below the first nesting;
        // multiplied by an enlarging outer portal's scale.
        static double RenderRange(int layer, const Game::Immersive::Portal* through,
                                  int renderDistanceChunks);
        // PortalRenderer.getPortalRenderDistance: the render distance a
        // view through `portal` draws its far world with.
        static int PortalRenderDistance(const Game::Immersive::Portal& portal, int renderDistanceChunks);

        // IPGlobal.portalRenderLimit: no more portals than this are rendered
        // in one frame, every layer counted. OBEY_PORTAL_RENDER_LIMIT=n
        // overrides.
        int m_portalRenderLimit = 200;
        // Top-level portals whose far side was drawn, for DrewLastFrame.
        std::unordered_set<Game::Immersive::PortalId> m_drawnThisFrame, m_drawnLastFrame;

        // Far levels drawn this frame: their mesh scheduling runs ONCE, after
        // the whole portal pass, not once per portal into them (it was up to
        // 19 runs and 20 ms in a frame).
        struct FarLevelPending { bool pending = false; glm::vec3 camera{0.0f}; };
        FarLevelPending m_farLevels[Game::kDimensionCount];
        void RenderCrossers(int layer, const Game::Immersive::Portal* through,
                            const Game::Immersive::Portal* outerThrough,
                            const Camera& camera, const glm::mat4& view, const glm::mat4& projection,
                            const Frustum& frustum, int renderDistanceChunks, float partialTick);
        const LevelRenderFn* m_renderCrossers = nullptr;
        void RenderLayer(int layer, const Game::Immersive::Portal* through,
                         const Game::Immersive::Portal* outerThrough,
                         const Camera& camera, const glm::mat4& view, const glm::mat4& projection,
                         const Frustum& frustum, float aspect, int renderDistanceChunks, float partialTick,
                         const LevelRenderFn& renderLevel);

        // `model` feeds uModel (world position for the fog overlay);
        // `outlineMode` 0 = solid fill (mask/depth passes), 3 = fog overlay.
        void DrawSurface(const SurfaceMesh& mesh, const PipelineState& state,
                         const glm::mat4& mvp, const glm::vec3& color,
                         const glm::mat4& model, float outlineMode = 0.0f);
        // The near world's fog over the far view, inside the mask: a
        // window at a distance is fogged like the wall around it.
        void DrawFogOverlay(const SurfaceMesh& mesh, const glm::mat4& model, const glm::mat4& mvp,
                            const Camera& camera, int innerLayer);
        static glm::mat4 SurfaceModel(const Game::Immersive::Portal& portal);
        static PipelineState BaseState();

        // While a layer's far side is drawn, ChunkRenderer's per-pass
        // SetPipelineState calls must inherit the mask — see
        // RenderBackend::SetStencilOverride.
        static void SetLayerOverride(int layer);
    };

    extern ImmersivePortalRenderer g_immersivePortalRenderer;

} // namespace Render

#endif // ENABLE_IMMERSIVE_PORTALS
