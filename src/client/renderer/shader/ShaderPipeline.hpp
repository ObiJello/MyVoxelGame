// File: src/client/renderer/shader/ShaderPipeline.hpp
//
// Runs a Minecraft shader pack over the engine's frame, in Minecraft's
// order:
//   * the shadow pass — the terrain drawn from the sun with the pack's
//     shadow program into shadowtex0/1 and shadowcolor0;
//   * the gbuffers programs — gbuffers_terrain over the opaque and cutout
//     terrain passes, gbuffers_water over the translucent one,
//     gbuffers_entities over mobs, players, items and other entities,
//     gbuffers_block over block entities, gbuffers_skytextured (or
//     skybasic) over the sky, gbuffers_clouds over the clouds and
//     gbuffers_textured over particles, with Iris's fallback chain when a
//     program is missing. Each is drawn from the engine's own vertex data
//     (ShaderPackGlsl decodes it into the attributes packs read) into the
//     pack's gbuffer set with the engine's depth; the engine's renderers
//     are untouched — the backend substitutes the programs at bind time;
//   * the deferred passes between the opaque and translucent stages,
//     depthtex1 snapshotted before the translucents, depthtex2 before the
//     hand;
//   * the composite passes and final over the colortex ping-pong buffers.
// The hand is still the engine's (it is drawn after the composites). The
// pack's shaders.properties supplies per-program alpha tests and blend
// modes; its options (Shaders::Options) are applied to the sources.
//
// Greedy meshing is turned off while a pack is loaded: a pack's attributes
// are per vertex, and a merged rectangle has no per-vertex colour, uv or
// block. The user's setting is restored on unload. Nothing in here runs
// while no pack is loaded — one Active() check per hook, and the backend's
// override mode is off.
//
// OpenGL only: the packs are GLSL and are compiled at runtime after a
// core-profile translation. On Vulkan the screen still lists packs, and
// applying one reports that it needs the OpenGL backend.
#pragma once

#include "../backend/RenderTypes.hpp"
#include "ShaderExpr.hpp"
#include "client/shader/ShaderOptions.hpp"

#include <array>
#include <glm/glm.hpp>
#include <map>
#include <string>
#include <vector>

namespace Render {

    // What the pack's uniforms are built from, gathered by the frame loop.
    struct ShaderFrameInput {
        glm::mat4  projection{1.0f};
        glm::mat4  view{1.0f};           // the frame's render-space view matrix
        glm::dvec3 cameraPosition{0.0};
        float      nearPlane = 0.05f;
        float      farPlane  = 1000.0f;
        float      sunAngleDeg = 0.0f;   // 0 = noon, EnvironmentFrame convention
        int        moonPhase = 0;
        glm::vec3  skyColor{0.0f};
        glm::vec3  fogColor{0.0f};
        float      fogStart = 0.0f;
        float      fogEnd   = 1024.0f;
        float      skyBrightness = 1.0f; // terrain dim, 0.2667..1
        long long  dayTime  = 6000;      // world day time, ticks
        int        isEyeInWater = 0;     // 0 air, 1 water, 2 lava
        int        eyeSkyLight = 15;     // sky light at the eye, 0..15 (MC eyeBrightness.y / 16)
        int        eyeBlockLight = 0;    // block light at the eye, 0..15
        float      deltaSeconds = 0.0f;
        bool       hideGui = false;
        int        dimension = 0;        // DimensionId: -1 nether, 0 overworld, 1 end
    };

    class ShaderPipeline {
    public:
        static ShaderPipeline& Get();

        // Load the selected pack (Shaders::Selected()); "" unloads. Safe at
        // any time outside BeginScene..EndScene.
        void ApplySelected();

        // Load one pack from its shaders directory (Shaders::Prepare). False
        // with a reason on failure; the pipeline is then off.
        bool Load(const std::string& shadersDir, std::string& error);
        void Unload();

        bool Active() const { return m_active; }
        // One line for the screen: "Off", "<name>: N passes", or the error.
        const std::string& Status() const { return m_status; }

        // Around the level render. BeginScene binds the offscreen target
        // (recreating it on a size change), clears the pack's buffers and
        // turns the backend's shader overrides on; SetFrameInput (before
        // the level draws) renders the shadow pass and sets the frame's
        // uniforms on the gbuffers programs; EndScene runs the composite
        // passes and leaves the backbuffer bound with the result. All
        // no-ops while inactive.
        // While suspended (the leave capture's panorama faces), the pack's
        // overrides are lifted and the frame renders with the engine's
        // shaders into the window; BeginScene is a no-op.
        void SetSuspended(bool suspended);
        void BeginScene(int width, int height);
        void SetFrameInput(const ShaderFrameInput& in);
        void EndScene(const ShaderFrameInput& in);

    private:
        ShaderPipeline() = default;

        struct Program {
            std::string      name;
            ShaderHandle     shader = INVALID_SHADER;
            bool             ownsShader = true;
            std::vector<int> drawBuffers;       // colortex indices written, in gl_FragData order
            RenderTargetHandle target = INVALID_RENDER_TARGET;
            bool             isFinal = false;
            float            alphaRef = 0.1f;   // shaders.properties alphaTest
        };
        struct ColorBuffer {
            TextureFormat format = TextureFormat::RGBA8;
            TextureHandle main = INVALID_TEXTURE;
            TextureHandle alt  = INVALID_TEXTURE;
            bool          used = false;         // referenced by any pass
            bool          clear = true;         // colortexNClear: false keeps last frame's content
            glm::vec4     clearColor{0.0f};     // colortexNClearColor
        };
        // One clear per distinct clear colour over the cleared colortex1..7
        // mains (colortex0 takes the frame loop's own clear).
        struct ClearGroup {
            RenderTargetHandle target = INVALID_RENDER_TARGET;
            glm::vec4          color{0.0f};
        };
        static constexpr int kColorBuffers = 8;

        // The gbuffers programs, by the engine renderer family they stand
        // in for. Terrain and water go through the chunk renderer's pass
        // overrides; the rest through the backend's shader overrides.
        enum Family {
            kFamTerrain = 0, kFamWater, kFamEntities, kFamBlock, kFamSky, kFamClouds, kFamParticles,
            kFamShadow, kFamilyCount
        };

        // Which texture-unit layout a program is drawn with: the composite
        // quad owns every unit; a gbuffers program shares units 0..2 with
        // the engine's renderers (atlas or texture, sprite table, face map).
        enum class Layout { Composite, Gbuffers };

        bool ReadPackFile(const std::string& rel, std::string& out) const;
        bool CompileProgram(const std::string& dir, const std::string& name, bool gbuffers, bool entityLayout,
                            Program& out, std::string& error);
        bool LoadPrograms(const std::string& dir, std::string& error);
        void DestroyPrograms();
        void ParseProperties();
        std::string DimensionFolder(int dimension) const;
        void CreateBuffers(int width, int height);
        void DestroyBuffers();
        void CreateShadowBuffers();
        void DestroyShadowBuffers();
        void RebuildGbufferTargets();
        void EnsureQuad();
        void EnsureStaticTextures();
        void UpdateLightmap(float skyBrightness);
        void SetSamplerUniforms(const Program& p, Layout layout);
        void SetUniforms(const Program& p, const ShaderFrameInput& in, Layout layout);
        void BindInputs(Layout layout);
        void DrawPass(const Program& p, const ShaderFrameInput& in);
        void RunPasses(std::vector<Program>& passes, const ShaderFrameInput& in);
        void InstallOverrides();
        void RemoveOverrides();
        void RenderShadowPass(const ShaderFrameInput& in);
        void RunDeferred();
        void ProbeCentre(const char* what);
        void ProbeTexture(TextureHandle tex, const char* what);
        void ProbeThree(const char* what);
        void ProbeNormals(const ShaderFrameInput& in);
        bool ProgramEnabled(const std::string& name) const;
        bool OptionIsOn(const std::string& name) const;

        bool        m_active = false;
        std::string m_status = "Off";
        std::string m_packRoot;
        std::string m_packName;
        std::string m_dir;                      // "", "world0/", "world-1/", "world1/"
        int         m_dimension = 0;
        std::map<std::string, std::string> m_props;   // shaders.properties
        // shaders.properties `uniform.<type>.<name> = expr` and
        // `variable.<type>.<name> = expr`, in file order (a later one may
        // use an earlier one). Evaluated once per frame (ShaderExpr).
        struct CustomUniform {
            std::string type;      // bool int float vec2 vec3 vec4
            std::string name;
            std::string expr;
            bool        variable = false;   // a variable: for later expressions only
            ShaderExpr::Value value;
            bool        failed = false;     // logged once
        };
        std::vector<CustomUniform> m_customUniforms;
        ShaderExpr::SmoothState    m_smoothState;
        void EvaluateCustomUniforms(const ShaderFrameInput& in);
        // `#if` / `#ifdef` / `#else` / `#endif` over a .properties file with
        // the pack's option defines and the MC_* macros, as Iris does.
        std::string PreprocessProperties(const std::string& text) const;
        std::map<std::string, float> m_optionValues;   // every option, numeric where it is

        // block.properties: `block.<id> = name name:prop=value ...`, in file
        // order; and the atlas-space map built from it (EnsureEntityMap):
        // one RGBA8 texel per 4x4 atlas pixels carrying the id of the block
        // whose sprite covers it and the sprite's row in the sprite table,
        // what the terrain vertex prelude reads as mc_Entity and
        // mc_midTexCoord.
        struct BlockMapping { int id = 0; std::vector<std::string> entries; };
        std::vector<BlockMapping> m_blockMappings;
        bool          m_haveBlockProperties = false;
        void ParseBlockProperties();
        void EnsureEntityMap();
        TextureHandle m_entityMap = INVALID_TEXTURE;
        TextureHandle m_entityMapAtlas = INVALID_TEXTURE;   // the sprite table it was built for
        int           m_entityMapW = 0, m_entityMapH = 0;

        // MC eyeBrightnessSmooth: eyeBrightness eased with a half-life.
        glm::vec2 m_eyeBrightnessSmooth{0.0f, 240.0f};
        bool      m_haveEyeSmooth = false;
        std::map<std::string, std::string> m_options; // the player's option choices
        std::map<std::string, bool> m_toggles;        // every toggle option, as currently set
        std::vector<Program> m_passes;          // composite* then final
        std::vector<Program> m_deferred;        // deferred*
        Program     m_gbuffers[kFamilyCount];
        int         m_gbufferCount = 0;
        std::vector<ShaderHandle> m_familyEngineShaders[kFamilyCount];   // the engine's programs each family replaces

        ColorBuffer  m_color[kColorBuffers];
        TextureHandle m_depth  = INVALID_TEXTURE;   // depthtex0, the live depth
        TextureHandle m_depth1 = INVALID_TEXTURE;   // depthtex1: before translucents
        TextureHandle m_depth2 = INVALID_TEXTURE;   // depthtex2: before the hand
        RenderTargetHandle m_sceneTarget  = INVALID_RENDER_TARGET;   // colortex0 + depth, the engine's own draws
        std::vector<ClearGroup> m_clearGroups;                       // colortex1..7 mains cleared per frame
        RenderTargetHandle m_depth1Target = INVALID_RENDER_TARGET;
        RenderTargetHandle m_depth2Target = INVALID_RENDER_TARGET;
        int m_width = 0, m_height = 0;
        bool m_inScene = false;
        bool m_inShadowPass = false;
        bool m_suspended = false;
        bool m_deferredRanThisFrame = false;
        bool m_abEngineTerrain = false;   // frames 40..43: terrain by the engine's shader (diagnostic A/B)

        // Shadow map.
        int           m_shadowRes = 0;
        float         m_shadowDistance = 160.0f;
        float         m_sunPathRotation = 0.0f;
        TextureHandle m_shadowDepth0 = INVALID_TEXTURE;   // shadowtex0: everything
        TextureHandle m_shadowDepth1 = INVALID_TEXTURE;   // shadowtex1: opaque only
        TextureHandle m_shadowColor0 = INVALID_TEXTURE;   // shadowcolor0
        RenderTargetHandle m_shadowTarget  = INVALID_RENDER_TARGET;
        RenderTargetHandle m_shadow1Target = INVALID_RENDER_TARGET;
        glm::mat4 m_shadowView{1.0f};        // camera at the origin, as the uniform
        glm::mat4 m_shadowProjection{1.0f};
        bool      m_haveShadow = false;

        MeshHandle   m_quad   = INVALID_MESH;
        BufferHandle m_quadVb = INVALID_BUFFER;
        BufferHandle m_quadIb = INVALID_BUFFER;
        TextureHandle m_noise = INVALID_TEXTURE;
        TextureHandle m_whiteDepth = INVALID_TEXTURE;   // shadowtex* without a shadow pass
        TextureHandle m_white = INVALID_TEXTURE;        // shadowcolor*, specular
        TextureHandle m_flatNormal = INVALID_TEXTURE;   // normals: (0.5, 0.5, 1)
        TextureHandle m_lightmap = INVALID_TEXTURE;     // 16x16, MC LightTexture
        std::array<uint8_t, 16 * 16 * 4> m_lightmapTexels{};
        bool m_lightmapUploaded = false;
        ShaderHandle  m_blit = INVALID_SHADER;

        bool m_greedyWasEnabled = true;

        // Per-frame uniform state.
        ShaderFrameInput m_frame;
        int       m_frameCounter = 0;
        float     m_frameTimeCounter = 0.0f;
        glm::mat4 m_prevModelView{1.0f};
        glm::mat4 m_prevProjection{1.0f};
        glm::dvec3 m_prevCameraPosition{0.0};
        bool      m_havePrev = false;
    };

} // namespace Render
