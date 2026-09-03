// File: src/client/renderer/environment/SkyRenderer.hpp
//
// Minecraft's sky, ported 1:1 from SkyRenderer.java (post-1.21.9):
// sky disc → sunrise/sunset fan → sun → moon (8 phases) → stars → dark disc.
// Drawn camera-centered (rotation-only view) right after the clear, before
// terrain; all passes depth-test/write OFF so terrain draws over it.
// Colors/angles come from Render::EnvironmentState (updated once per frame).
#pragma once

#include "../backend/RenderTypes.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Render {

    // A selectable sky. "vanilla" = procedural MC sky, "end" = the End
    // skybox, anything else = a 6-face cubemap set: a folder under
    // assets/textures/environment/skyboxes/<id>/panorama_0..5.png (drop in
    // custom/space skyboxes there) or one of the title-screen panorama sets.
    struct SkyboxInfo {
        std::string id;
        std::string label;
    };
    std::vector<SkyboxInfo> DiscoverSkyboxes();

    class SkyRenderer {
    public:
        SkyRenderer() = default;
        ~SkyRenderer();

        bool Initialize();
        void Shutdown();

        // Select the active sky for this session. mode: 0 = static
        // (End-style), 1 = darkens at night, 2 = darkens + sun/moon/stars
        // drawn on top. Unknown/missing sets fall back to "vanilla".
        // Also installs the skybox fog-color override on EnvironmentState.
        void SetSkybox(const std::string& id, int mode);
        const std::string& CurrentSkybox() const { return m_userSkyboxId; }
        int CurrentSkyboxMode() const { return m_userSkyboxMode; }

        // Which dimension the local player is in — raw Game::DimensionId
        // (-1 nether, 0 overworld, 1 end). Driven by ChangeDimensionS2C.
        //
        // The sky is a property of the dimension in MC
        // (DimensionSpecialEffects.SkyType), not of the player's settings: the
        // End has its static starfield, the Nether has NO sky at all and shows
        // only fog, and the player's chosen skybox applies to the overworld.
        // Selecting it per dimension is why the End looked like a blue
        // overworld sky — the End skybox already existed and nothing ever
        // asked for it.
        void SetDimension(int rawDimensionId);

        // MC DimensionSpecialEffects.NetherEffects — the Nether draws no sky,
        // so everything is the fog colour. This is nether_wastes' fog
        // (#330808, data/minecraft/worldgen/biome/nether_wastes.json), which
        // is what most of the Nether shows; per-biome fog needs a biome-aware
        // fog system this engine does not have yet.
        static constexpr float kNetherFog[3] = {0x33 / 255.0f, 0x08 / 255.0f, 0x08 / 255.0f};

        // True while the active dimension draws no sky (the Nether). The
        // cloud renderer reads it — MC has no clouds outside the overworld.
        bool SkyHidden() const { return m_noSky; }

        // True while the End's starfield is the active sky. Distinct from the
        // player having CHOSEN the "end" skybox in settings — that is a purely
        // cosmetic overworld choice and should not suppress clouds.
        bool CurrentSkyboxIsEnd() const { return m_dimension == 1; }

        // proj: dedicated sky projection (far plane must cover the 512-radius
        //       disc — the main projection's far plane is too near at low
        //       render distance).
        // viewRotation: the camera view matrix with translation stripped.
        void Render(const glm::mat4& proj, const glm::mat4& viewRotation);

        // The sky of `rawDimensionId` from this viewpoint, whatever the
        // active dimension is — the far side of a portal. The Nether draws
        // nothing (fog only), the End its starfield, the Overworld the
        // vanilla procedural sky (a cubemap chosen in settings is only
        // resident while the Overworld is active). The caller installs the
        // matching EnvironmentState frame override first.
        void RenderForDimension(int rawDimensionId, const glm::mat4& proj,
                                const glm::mat4& viewRotation);

    private:
        struct Vertex {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(Vertex) == 24, "must match GetBlockVertexLayout stride");

        struct Mesh {
            BufferHandle vb = INVALID_BUFFER;
            BufferHandle ib = INVALID_BUFFER;
            MeshHandle mesh = INVALID_MESH;
            uint32_t indexCount = 0;
        };

        Mesh CreateMesh(const void* verts, size_t vertBytes,
                        const uint32_t* indices, size_t indexCount);
        void DestroyMesh(Mesh& m);
        void BuildSkyDiscs();
        void BuildSunriseFan();
        void BuildCelestialQuads();
        void BuildStars();
        void BuildSkyboxCubes();
        void DestroySkyboxTextures();
        bool LoadSkyboxTextures(const std::string& id);
        void RenderSkybox(const glm::mat4& viewProj, float brightness);

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        ShaderHandle m_shader = INVALID_SHADER;
        TextureHandle m_whiteTexture = INVALID_TEXTURE;
        TextureHandle m_sunTexture = INVALID_TEXTURE;
        TextureHandle m_moonTexture = INVALID_TEXTURE;

        Mesh m_topDisc;     // y = +16, sky color
        Mesh m_bottomDisc;  // y = -16, black (dark disc)
        Mesh m_sunriseFan;  // 18-vertex glow fan
        Mesh m_sunQuad;
        Mesh m_moonQuads;   // 8 phase quads, drawn 6 indices at phase*6
        Mesh m_stars;       // ~1500 quads from JavaRandom(10842)

        // Cubemap skybox state.
        Mesh m_skyboxCube;  // inward cube, UV 0..1 per face (panorama sets)
        Mesh m_endCube;     // inward cube, UV 0..16 (MC buildEndSky tiling)
        TextureHandle m_skyboxFaces[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                          INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
        TextureHandle m_endTexture = INVALID_TEXTURE;
        std::string m_skyboxId = "vanilla";
        int m_skyboxMode = 2;
        bool m_skyboxValid = false;  // textures loaded, cube path active
        bool m_skyboxIsEnd = false;
        // Loads end_sky.png if it is not resident (a far-side End view while
        // the active sky is another dimension's).
        bool EnsureEndTexture();

        // What the PLAYER chose, kept apart from what is currently ACTIVE so
        // that a trip to the End does not overwrite their setting — and so
        // coming home restores it.
        std::string m_userSkyboxId = "vanilla";
        int         m_userSkyboxMode = 2;
        int         m_dimension = 0;
        bool        m_noSky = false;

        // Apply m_dimension + the user's choice to the active sky.
        void ApplyDimensionSky();
        // The old body of SetSkybox: load textures and install the fog
        // override for one skybox id.
        void ApplySkybox(const std::string& id, int mode);
        glm::vec3 m_skyboxFogColor{0.5f};

        bool m_initialized = false;
    };

    extern SkyRenderer g_skyRenderer;

} // namespace Render
