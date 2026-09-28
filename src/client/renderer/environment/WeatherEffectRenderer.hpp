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
// (camera-relative rendering, RenderOrigin.hpp). One streaming vertex buffer
// per draw from a ring of slots; a slot is only ever regrown with the
// deferred destroys (Vulkan frame overlap).
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

        // Extract, prepare and draw the columns for this view. `cameraPos` is
        // the eye in world doubles; `partialTick` the client tick fraction.
        // Draws nothing unless the dimension can have weather and the rain
        // level is above 0. This form draws the BOUND level (the active one
        // outside a portal view) under the current portal clip plane.
        void Render(const glm::mat4& proj, const glm::mat4& view,
                    const glm::dvec3& cameraPos, float partialTick);
        // A portal view's form: `dimension` is the level the view shows (its
        // level bound — the heightmaps and biomes read the globals), and
        // `clipPlane` the view's clip plane in render space (zero = none).
        // The eye and the render origin are the view's.
        void Render(const glm::mat4& proj, const glm::mat4& view,
                    const glm::dvec3& cameraPos, float partialTick,
                    Game::DimensionId dimension, const glm::vec4& clipPlane);

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

        void PrepareInstances(std::vector<Vertex>& out, const std::vector<ColumnInstance>& columns,
                              const glm::dvec3& cameraPos, float maxAlpha, int radius, float intensity) const;

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

        // A ring of streaming vertex buffers, one per draw. A slot reused a
        // few frames later is free on both backends; growing one defers the
        // old buffer's destruction past the frames still reading it. Sized
        // for several draws a frame (the main view plus every portal view)
        // across the frames in flight.
        struct StreamSlot {
            BufferHandle vb   = INVALID_BUFFER;
            MeshHandle   mesh = INVALID_MESH;
            size_t       capacityVerts = 0;
        };
        static constexpr size_t kStreamSlots = 24;
        std::array<StreamSlot, kStreamSlots> m_slots;
        size_t m_slotCursor = 0;
        StreamSlot& AcquireSlot(size_t vertsNeeded);
        void DestroySlots();
    };

    extern WeatherEffectRenderer g_weatherEffectRenderer;

} // namespace Render
