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
        const std::string& CurrentSkybox() const { return m_skyboxId; }
        int CurrentSkyboxMode() const { return m_skyboxMode; }

        // proj: dedicated sky projection (far plane must cover the 512-radius
        //       disc — the main projection's far plane is too near at low
        //       render distance).
        // viewRotation: the camera view matrix with translation stripped.
        void Render(const glm::mat4& proj, const glm::mat4& viewRotation);

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
        glm::vec3 m_skyboxFogColor{0.5f};

        bool m_initialized = false;
    };

    extern SkyRenderer g_skyRenderer;

} // namespace Render
