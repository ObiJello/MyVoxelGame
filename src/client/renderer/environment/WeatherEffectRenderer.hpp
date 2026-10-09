// File: src/client/renderer/environment/WeatherEffectRenderer.hpp
//
// MC net.minecraft.client.renderer.WeatherEffectRenderer — the rain and snow
// "sheets": one camera-facing quad per block column around the camera,
// textured with environment/rain.png or snow.png scrolling down (rain) or
// drifting (snow).
//
// Ported as MC has it:
//   * extractRenderState — every column within weatherRadius (the Weather
//     Radius option, 3..10) where the rain level is above 0: from
//     max(camY - r, MOTION_BLOCKING height) to max(camY + r, that height),
//     skipped when empty; ClientLevel.getPrecipitationAt at the camera's
//     height picks rain or snow (ClientWeather::BiomePrecipitationAt); a
//     per-column RandomSource seeded from x/z; the light from
//     LightCoordsUtil.getLightCoords at max(camY, height) — brightened for
//     snow ((l * 3 + 15) / 4 on both channels).
//   * createRainColumnInstance / createSnowColumnInstance — the scrolling
//     V offset (3 + nextFloat blocks per 32 ticks, the column's tick offset)
//     and snow's gaussian drift in U and V.
//   * prepareInstances — the 32x32 column-orientation table (each quad faces
//     the camera column), alpha lerped from maxAlpha (1 rain, 0.8 snow) at
//     the camera to 0.5 at the radius, times the rain level; V = y / 4 +
//     offset.
//   * render — MC's WEATHER pipeline: the particle shader (texture x vertex
//     colour x lightmap, 0.1 cutout, fog), TRANSLUCENT blend, depth tested,
//     no depth write, no culling; rain columns, then snow columns.
//
// The lightmap is applied per column on the CPU (the colour of its packed
// light through the frame's lightmap, EntityEnvironment::LightColor) and
// carried in the vertex colour — the shader's own light is 1 — so the
// particle shader is reused unchanged on both backends.
//
// Positions are world-space doubles narrowed through Render::ToRender
// (camera-relative rendering, RenderOrigin.hpp). One streaming buffer per
// draw from a ring of slots; a slot is only ever regrown with the deferred
// destroys (Vulkan frame overlap).
//
// The streak path (the default; OBEY_RAIN_STREAKS=0 restores MC's quads):
// a column quad launches a fragment for every pixel it covers and ~97 % of
// them fail the texture's alpha cutout — on a tile GPU that pass is bound
// by fragment LAUNCHES, not shading (12 columns deep over the screen at
// radius 10). So the texture's alpha > 0 texels are rectangled once at load
// (StreakSet, ~110 rects for rain.png) and each column is drawn as those
// rects, instanced per column and texture repeat, placed by
// weather_streak(_vk).vert exactly where the quad would have sampled them:
// the same pixels get the same texels. The fragment shaders are unchanged.
// A texture whose alpha is dense (a pack's) falls back to the quads.
//
// Main thread only, against the bound (active) client level.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Render {

    class WeatherEffectRenderer {
    public:
        WeatherEffectRenderer();
        ~WeatherEffectRenderer();

        bool Initialize();
        void Shutdown();
        // Resource pack reload: rain.png / snow.png again.
        void ReloadTextures();

        // Whether Rain Resolution: Half exists on this backend. Not on
        // Metal: with the streak instancing the full-resolution pass is
        // the cheaper one there (ABBA on the tour, 2026-10-07: Full 217 fps
        // / 4.61 ms, Half 210 fps / 4.77 ms — Half's own target pass, the
        // composite and the previous-depth reprojection cost more than the
        // fragments it still saves), so the setting is ignored and Video
        // Settings shows no control. OpenGL and Vulkan keep the option.
        static bool HalfResolutionAvailable();

        // Extract, prepare and draw the columns for this view. `cameraPos` is
        // the eye in world doubles; `partialTick` the client tick fraction.
        // Draws nothing unless the dimension can have weather and the rain
        // level is above 0. This form draws the BOUND level (the active one
        // outside a portal view) under the current portal clip plane.
        // `allowHalfResolution`: the main view's pass may honour Rain
        // Resolution: Half (OpenGL: DrawHalfResolution; Vulkan: the rain
        // RenderAhead drew, laid over the frame here); every other caller —
        // portal views, the OIT stages, the panorama faces — draws at full
        // resolution, under the stencils, clip planes and viewports it set.
        void Render(const glm::mat4& proj, const glm::mat4& view,
                    const glm::dvec3& cameraPos, float partialTick, bool allowHalfResolution = false);
        // A portal view's form: `dimension` is the level the view shows (its
        // level bound — the heightmaps and biomes read the globals), and
        // `clipPlane` the view's clip plane in render space (zero = none).
        // The eye and the render origin are the view's.
        void Render(const glm::mat4& proj, const glm::mat4& view,
                    const glm::dvec3& cameraPos, float partialTick,
                    Game::DimensionId dimension, const glm::vec4& clipPlane,
                    bool allowHalfResolution = false);
        // Vulkan, the main view, once a frame after its camera and render
        // origin are final and before anything is drawn into the frame:
        // Rain Resolution: Half drawn ahead of the frame's render pass from
        // the previous frame's depth (see DrawAhead); the main view's
        // Render then only lays it over the frame. Also where the frame is
        // marked for it: depth kept and handed to the next frame, the
        // viewmodels and GUI items in depth bands (LateDepthBands.hpp).
        void RenderAhead(const glm::mat4& proj, const glm::mat4& view,
                         const glm::dvec3& cameraPos, float partialTick);

    private:
        // MC WeatherEffectRenderer.ColumnInstance.
        struct ColumnInstance {
            int   x, z;
            int   bottomY, topY;
            float uOffset, vOffset;
            int   lightCoords;
        };

        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "must match GetBlockVertexLayout stride");

        void LoadTextures();
        void DestroyTextures(bool deferred);

        // ── Streaks (see the file header) ───────────────────────────────
        struct StreakRect { uint16_t x, y, w, h; };   // texels
        struct StreakSet {
            std::vector<StreakRect> rects;
            // The template: every rect's quad (`copies` of it, see the
            // shader) as 24-byte block-layout vertices + uint32 indices.
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            uint32_t     indexCount = 0;
            int          copies = 1;
            bool         usable = false;   // rects found and sparse enough to be worth it
        };
        // One column x one texture repeat (weather_streak_vk.vert's instance
        // attributes, locations 3..6).
        struct StreakInstance {
            float column[4];   // cx, cz, hx, hz (render space)
            float span[4];     // bottomY, topY (render space), uOffset, vOffset (+ origin term)
            float tint[4];     // rgba
            float repeat[4];   // R, 0, 0, 0
        };
        static_assert(sizeof(StreakInstance) == 64, "the instance layout's stride");
        // Greedy rectangles of the texture's alpha > 0 texels and the
        // template buffers; `copies` 2 for a texture whose u is offset per
        // column (snow), 1 otherwise. Unusable when there are none, or when
        // the alpha covers more than a quarter of the texture (a pack's
        // dense texture: the quads are cheaper than thousands of rects).
        void BuildStreakSet(StreakSet& set, const unsigned char* rgba, int width, int height, int copies);
        void DestroyStreakSet(StreakSet& set, bool deferred);
        // prepareInstances as instances: one per (column, repeat).
        void PrepareStreakInstances(std::vector<StreakInstance>& out, const std::vector<ColumnInstance>& columns,
                                    const glm::dvec3& cameraPos, float maxAlpha, int radius, float intensity) const;
        bool StreaksActive() const { return m_streaksWanted && m_rainStreaks.usable && m_snowStreaks.usable; }
        bool          m_streaksWanted = true;
        StreakSet     m_rainStreaks, m_snowStreaks;
        uint32_t      m_streakGeneration = 0;   // bumped when the sets are rebuilt (slots re-mesh)
        ShaderHandle  m_streakShader     = INVALID_SHADER;   // weather_streak.vert + the particle fragment
        ShaderHandle  m_streakHalfShader = INVALID_SHADER;   // weather_streak.vert + weather_half.frag
        VertexLayout  m_streakInstanceLayout;
        std::vector<StreakInstance> m_instances;
        bool CreateStreakShaders();

        void PrepareInstances(std::vector<Vertex>& out, const std::vector<ColumnInstance>& columns,
                              const glm::dvec3& cameraPos, float maxAlpha, int radius, float intensity) const;
        struct StreamSlot;
        // What one Render draws: the slot holding this view's data, and the
        // counts — vertices (MC's quads) or streak instances — of rain, then
        // snow.
        struct Batch {
            const StreamSlot* slot = nullptr;
            size_t rainCount = 0, snowCount = 0;
            bool   streaks = false;
        };
        // extractRenderState + prepareInstances for `dimension` around the
        // eye, uploaded into a stream slot: false when there is nothing to
        // draw (no weather there, or no column in range).
        bool BuildColumns(Game::DimensionId dimension, const glm::dvec3& cameraPos, float partialTick, Batch& out);
        void RestoreDefaultState();

        // MC's columnSizeX / columnSizeZ: the half-quad direction for each
        // column relative to the camera's, 32 x 32 around it.
        std::array<float, 1024> m_columnSizeX{};
        std::array<float, 1024> m_columnSizeZ{};

        ShaderHandle  m_shader      = INVALID_SHADER;
        TextureHandle m_rainTexture = INVALID_TEXTURE;
        TextureHandle m_snowTexture = INVALID_TEXTURE;

        std::vector<ColumnInstance> m_rainColumns;
        std::vector<ColumnInstance> m_snowColumns;
        std::vector<Vertex>         m_vertices;

        // A ring of streaming buffers, one per draw — the quads' vertices, or
        // the streak instances, with a mesh over each (the instanced ones
        // pair the buffer with the rain / snow templates). A slot reused a
        // few frames later is free on both backends; growing one defers the
        // old buffer's destruction past the frames still reading it. Sized
        // for several draws a frame (the main view plus every portal view)
        // across the frames in flight.
        struct StreamSlot {
            BufferHandle vb       = INVALID_BUFFER;
            MeshHandle   mesh     = INVALID_MESH;   // the quads (block layout)
            MeshHandle   rainMesh = INVALID_MESH;   // rain template + this buffer's instances
            MeshHandle   snowMesh = INVALID_MESH;
            uint32_t     meshGeneration = 0;        // m_streakGeneration the instanced meshes were made for
            size_t       capacityBytes = 0;
        };
        static constexpr size_t kStreamSlots = 24;
        std::array<StreamSlot, kStreamSlots> m_slots;
        size_t m_slotCursor = 0;
        StreamSlot& AcquireSlot(size_t bytesNeeded, bool streaks);
        void DestroySlots();

        // The columns' draw: the shader (the batch's kind, `half` for the
        // half-resolution fragment shader) bound with the world's uniforms,
        // `state` set, rain then snow from the batch; `sceneDepth` (half
        // resolution) at texture slot 1, and with `reprojection` it is the
        // previous frame's, seen through that frame's view-projection.
        void DrawColumns(bool half, const Batch& batch,
                         const glm::mat4& mvp, const glm::dvec3& cameraPos, const glm::vec4& clipPlane,
                         const PipelineState& state, TextureHandle sceneDepth = INVALID_TEXTURE,
                         const glm::mat4* reprojection = nullptr);

        // ── Rain Resolution: Half ───────────────────────────────────────
        // The columns drawn into a target half the world's size (one per
        // frame slot: a frame never writes the one the previous frame's
        // composite reads), depth-tested in the shader, colour premultiplied
        // so the layers accumulate as "over" (One, OneMinusSrcAlpha); one
        // full-screen draw then lays the target over the frame with the same
        // blend — the result the full-resolution pass gets layer by layer,
        // with a quarter of the pixels shaded. Not under Improved
        // Transparency (its OIT stages draw the weather).
        //  - Vulkan (DrawAhead, from RenderAhead): before the frame's render
        //    pass opens, against the PREVIOUS frame's depth reprojected
        //    (weather_half_vk.frag), so the frame's pass never breaks — a
        //    break alone costs ~0.6 ms at 3420x2146. Without that depth (the
        //    first frame of rain, a resize, something drawn first) the rain
        //    is drawn at full resolution in the frame instead.
        //  - OpenGL (DrawHalfResolution): FBO 0's depth copied at the
        //    weather's place in the frame (m_sceneDepthCopy), the target
        //    drawn, the frame resumed. False: nothing was drawn, the caller
        //    draws at full resolution.
        bool DrawHalfResolution(const Batch& batch, const glm::mat4& mvp, const glm::dvec3& cameraPos);
        bool DrawAhead(const Batch& batch, const glm::mat4& mvp, const glm::mat4& reprojection,
                       const glm::dvec3& cameraPos, TextureHandle previousDepth);
        // Into the bound half target: cleared, then the columns.
        void DrawIntoHalfTarget(const Batch& batch, const glm::mat4& mvp, const glm::dvec3& cameraPos,
                                TextureHandle sceneDepth, const glm::mat4* reprojection, int halfWidth, int halfHeight);
        // The half target over the frame (bound), `width` x `height`.
        void CompositeHalf(RenderTargetHandle target, int width, int height);
        bool EnsureHalfResources(int width, int height);
        void ReleaseHalfResources();
        ShaderHandle  m_halfShader      = INVALID_SHADER;
        ShaderHandle  m_compositeShader = INVALID_SHADER;
        BufferHandle  m_quadVB          = INVALID_BUFFER;
        MeshHandle    m_quadMesh        = INVALID_MESH;
        std::array<RenderTargetHandle, 2> m_halfTargets{INVALID_RENDER_TARGET, INVALID_RENDER_TARGET};
        int           m_halfWidth = 0, m_halfHeight = 0;
        TextureHandle m_sceneDepthCopy = INVALID_TEXTURE;   // OpenGL
        int           m_sceneDepthWidth = 0, m_sceneDepthHeight = 0;
        bool          m_halfFailed = false;
        // RenderBackend::FrameNumber() of the frame whose rain DrawAhead drew
        // (Render composites it), and the main view RenderAhead last saw —
        // the next frame's reprojection, if it is the very next frame.
        uint64_t   m_aheadFrame = 0;
        uint64_t   m_viewFrame  = 0;
        glm::mat4  m_viewMvp{1.0f};
        glm::dvec3 m_viewOrigin{0.0};
    };

    extern WeatherEffectRenderer g_weatherEffectRenderer;

} // namespace Render
