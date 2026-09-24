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
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Render {

    struct EnvironmentFrame;

    // A selectable sky. "vanilla" = procedural MC sky, "end" = the End
    // skybox, anything else = a 6-face cubemap set (a folder holding
    // panorama_0..5.png) from one of three places, searched in this order
    // so a player's own set can shadow a shipped one of the same name:
    //   User      <game dir>/skyboxes/<id>/          the player's downloads
    //   Builtin   assets/textures/environment/skyboxes/<id>/   shipped
    //   Panorama  assets/textures/gui/title/background/<id>/   title screens
    enum class SkyboxSource : uint8_t { Special, User, Builtin, Panorama };
    // What a set is made of:
    //   SixFace   panorama_0..5 images, drawn as an opaque cube in place
    //             of the vanilla sky (the picker's mode decides how it
    //             follows the day).
    //   OptiFine  a Minecraft resource pack with an OptiFine custom sky
    //             (assets/minecraft/optifine/sky/world0/sky<n>.properties,
    //             each with a 3×2 texture of the six faces): its layers are
    //             blended OVER the vanilla sky with the pack's own fades,
    //             rotation and blend mode, exactly as OptiFine draws them.
    //             Drop the unzipped pack folder into the skyboxes folder.
    enum class SkyboxKind : uint8_t { Special, SixFace, OptiFine };
    struct SkyboxInfo {
        std::string  id;
        std::string  label;
        SkyboxSource source = SkyboxSource::Special;
        SkyboxKind   kind   = SkyboxKind::Special;
        // Absolute directory of the set (trailing separator); empty for the
        // two special skies ("vanilla", "end"). For SixFace it holds the
        // faces; for OptiFine it is the pack root.
        std::string  dir;
        // OptiFine only: the first layer's 3×2 texture, for the preview.
        std::string  optifineTexture;
        int          optifineLayers = 0;
    };
    std::vector<SkyboxInfo> DiscoverSkyboxes();
    // The user folder DiscoverSkyboxes reads (created at startup), for the
    // picker's "open folder" button.
    std::string UserSkyboxDirectory();
    // The file for face `face` (0..5) of the set in `dir`: panorama_<face>
    // with any of the image extensions the loader reads (png, jpg, jpeg,
    // bmp, tga — a downloaded "skycube" is often BMP or TGA). Empty when
    // the set has no such face.
    std::string SkyboxFacePath(const std::string& dir, int face);
    // Fills kind/optifineTexture/optifineLayers for the set in `dir`;
    // false when the folder is not a set of either kind.
    bool DescribeSkyboxSet(const std::string& dir, SkyboxInfo& info);

    // An inclusive integer range from an OptiFine range list ("0-64 100-128
    // 200"); INT_MIN / INT_MAX stand for an open end ("64-", "-64").
    struct SkyIntRange { int lo = INT_MIN; int hi = INT_MAX; };

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
        // The active set is an OptiFine pack: its own fades and blend
        // stand in for the mode, which the settings screen then greys out.
        bool CurrentSkyboxIsOptiFine() const { return PackChosen(); }

        // Where the viewer stands, for an OptiFine sky's `biomes` and
        // `heights` rules (OptiFine reads the camera entity's block
        // position and the biome there). Set once a frame by the main loop.
        void SetObserver(const glm::ivec3& blockPos, uint16_t biomeId) {
            m_observerBlock = blockPos;
            m_observerBiome = biomeId;
        }

        // Resource pack reload: the sun, moon and End textures are read
        // again and the sky re-resolved (an enabled resource pack with an
        // OptiFine sky supplies the "Vanilla" sky's layers, as in OptiFine).
        void ReloadResources();

        // The chosen pack's own clouds.png (absolute path), or empty when
        // the chosen sky is not a pack or the pack has none. The main loop
        // hands it to the cloud renderer every frame.
        const std::string& PackCloudTexture() const;

        // ── Sky pack prefetch ──────────────────────────────────────────
        // An OptiFine sky pack is nine or so 3072×2048 PNGs: decoded and
        // uploaded on the join, they were a 330-390 ms hitch (Join.Skybox)
        // on the first world of a session. The title screen knows which
        // sky the player is likely to join with — the most recently played
        // world's, then whichever world is selected — so it asks for the
        // pack here: the layers decode on a background thread (two at a
        // time, so the title stays smooth), and PumpPrefetch, once a title
        // frame, uploads one decoded layer. A join then finds the pack
        // resident, or at worst decoded, and SetSkybox costs the uploads
        // alone. A prefetch for a sky that is already resident or already
        // in flight is a no-op; one for a different sky replaces the
        // current one. Nothing to decode (vanilla without a pack sky, a
        // six-face set, an unknown id) is a no-op too.
        void PrefetchSkybox(const std::string& id);
        // One title frame's share of the prefetch: one layer's six faces.
        void PumpPrefetch();

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

        // The Hush (DimensionId::Hush, raw 2): an open sky frozen at
        // midnight — MC's fixedTime on a NORMAL sky, which vanilla never
        // combines. Sculk's palette (#0E3A3F base, #2BD4C0 veins) taken down
        // to night: a teal-black fog the terrain fades into, a darker disc
        // behind it, and stars tinted toward the veins' cyan. All of it is
        // dark on purpose — the glowing flora and crystals carry the light.
        static constexpr float kHushFog[3]      = {0x0B / 255.0f, 0x2A / 255.0f, 0x2E / 255.0f};
        static constexpr float kHushSky[3]      = {0x06 / 255.0f, 0x14 / 255.0f, 0x1A / 255.0f};
        static constexpr float kHushStarTint[3] = {0.55f, 0.95f, 1.0f};

        // True while the active dimension draws no sky (the Nether). The
        // cloud renderer reads it — MC has no clouds outside the overworld.
        bool SkyHidden() const { return m_noSky; }

        // True while the End's starfield is the active sky. Distinct from the
        // player having CHOSEN the "end" skybox in settings — that is a purely
        // cosmetic overworld choice and should not suppress clouds.
        bool CurrentSkyboxIsEnd() const { return m_dimension == 1; }

        // Whether the active dimension has a cloud layer. MC gives every
        // dimension but the Overworld a cloud height of Float.NaN
        // (DimensionSpecialEffects: NetherEffects and EndEffects both;
        // the Hush is a night sky with none) — its way of saying "no cloud
        // layer". The cloud passes gate on this, not on the sky's shape.
        // The Aether (raw 4) has one, at its own height and colour
        // (AetherSkyRenderEffects.renderClouds — EnvironmentFrame
        // .cloudBottomY / cloudColor); the Twilight Forest (raw 3) has none —
        // its dimension sets no CLOUD_COLOR, whose default is transparent.
        bool HasClouds() const { return m_dimension == 0 || m_dimension == 4; }

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
        // SkyRenderer.buildStars' loop with `attempts` tries from
        // JavaRandom(10842): 1500 for vanilla, 3000 for the Twilight Forest
        // (TFSkyRenderer.buildStars — the same stream, so its first half is
        // vanilla's sky).
        Mesh BuildStarMesh(int attempts);
        void BuildSkyboxCubes();
        void DestroySkyboxTextures();
        bool LoadSkyboxTextures(const std::string& id);
        void RenderSkybox(const glm::mat4& viewProj, float brightness);
        // The Hush's sky: the disc in kHushSky under the sky fog, then the
        // star sphere — tinted, additive, and at ONE fixed rotation (a
        // frozen sky, not the Overworld's turning one). No sunrise fan, sun,
        // moon, OptiFine layers or dark disc.
        void RenderFixedNight(const glm::mat4& proj, const glm::mat4& viewRotation);
        // TF TFSkyRenderer.renderSky: the vanilla overworld pass without the
        // sunrise fan, sun and moon — the sky disc in the frame's (biome) sky
        // colour, the doubled star field at full brightness turned only by
        // the −90° yaw, and the dark disc below the dimension's floor.
        void RenderTwilight(const glm::mat4& proj, const glm::mat4& viewRotation);

        // ── OptiFine custom sky ────────────────────────────────────────
        // One sky<n>.properties = one layer: six face textures cut from
        // its 3×2 image, a blend mode, a day-time fade, a rotation, and
        // the conditions OptiFine's CustomSkyLayer checks every frame
        // (biomes, heights, days, weather).
        struct OptiFineLayer {
            TextureHandle faces[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                      INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
            // OptiFine blend names: add (default), alpha, subtract, multiply,
            // dodge, burn, screen, overlay, replace.
            std::string blend = "add";
            bool fadeAlwaysOn = true;
            int  startFadeIn = 0, endFadeIn = 0, startFadeOut = 0, endFadeOut = 0;   // ticks
            bool  rotate = true;
            float speed  = 1.0f;
            glm::vec3 axis{0.0f, 0.0f, 1.0f};
            // Conditions; an empty list is no condition.
            std::vector<uint16_t>    biomes;          // biome ids the layer shows in
            bool                     biomesNegated = false;   // "biomes=!..." — shows everywhere else
            std::vector<SkyIntRange> heights;         // viewer block Y
            std::vector<SkyIntRange> days;            // day index within daysLoop
            int                      daysLoop = 8;
            bool weatherClear = true, weatherRain = false, weatherThunder = false;
            float transitionSec = 1.0f;               // biome/height brightness smoothing
            // Smoothed biome/height brightness (OptiFine's SmoothFloat).
            float positionBrightness = 1.0f;
            bool  positionInit = false;
            std::chrono::steady_clock::time_point lastEval{};
        };
        // A pack: its overworld (world0) and End (world1) layers, plus the
        // sun/moon/stars switches its Nuit / FabricSkyBoxes definition
        // carries when it has one (OptiFine has no such switch, but that
        // is how the pack author meant the sky to look).
        struct OptiFinePack {
            std::string id;                        // the skybox id it was loaded for
            std::string dir;                       // pack root, trailing separator
            std::vector<OptiFineLayer> layers[2];  // [0] overworld, [1] End
            bool showSun[2]   = {true, true};
            bool showMoon[2]  = {true, true};
            bool showStars[2] = {true, true};
            // The pack's replacements for the vanilla environment textures
            // (assets/minecraft/textures/environment/...), when it ships
            // them: sun.png, moon_phases.png (same 4×2 phase grid at any
            // size) and clouds.png (given to the cloud renderer by path).
            TextureHandle sunTexture  = INVALID_TEXTURE;
            TextureHandle moonTexture = INVALID_TEXTURE;
            std::string   cloudTexturePath;
            bool loaded = false;
        };
        OptiFinePack m_pack;
        // The ACTIVE overworld sky is the pack's world0 layers over the
        // vanilla sky. False in the End and the Nether, and while a
        // far-side End view is being drawn; the pack itself stays resident
        // (its End layers ride over the End starfield).
        bool m_skyboxIsOptiFine = false;
        // The player's chosen sky is the resident pack.
        bool PackChosen() const { return m_pack.loaded && m_pack.id == m_userSkyboxId; }
        bool LoadOptiFinePack(const std::string& id, const std::string& setDir);
        void ReadPackDecorations(const std::string& setDir);
        void DestroyOptiFinePack();

        // A pack between the disk and the GPU. Decoding (the PNG work,
        // which is all of the cost) is thread-safe and touches nothing of
        // the renderer; uploading is the main thread's, one layer at a time
        // or all at once. Slots keep the file order the layers draw in
        // whatever order the workers finish them.
        struct DecodedImage {
            std::vector<unsigned char> pixels;   // RGBA8
            int w = 0, h = 0;
        };
        struct DecodedLayer {
            OptiFineLayer layer;                          // faces INVALID until uploaded
            std::array<std::vector<unsigned char>, 6> pixels;
            int  faceSize = 0;                            // 0: the layer could not be read
            bool blur     = false;                        // .mcmeta blur → linear sampling
            bool uploaded = false;
        };
        struct DecodedPack {
            std::string id;                               // the skybox id it is for
            std::string dir;                              // pack root, trailing separator
            std::mutex  mutex;                            // guards the slots and `decoded`
            std::vector<DecodedLayer> layers[2];          // [0] overworld, [1] End; sized up front
            std::vector<uint8_t>      ready[2];           // slot decoded
            DecodedImage sun, moon;                       // the pack's own, when it has them
            bool decoded = false;                         // every slot is in
            std::atomic<bool> cancel{false};              // abandoned: stop after the current layer
        };
        // Reads one sky<n>.properties and cuts its 3×2 image into faces.
        static void DecodeOptiFineLayer(const std::string& file, const std::string& setDir,
                                        const std::string& skyDir, DecodedLayer& out);
        // Decodes every layer of both worlds with `parallelism` workers,
        // then the pack's sun and moon. Runs on any thread.
        static void DecodeOptiFinePack(DecodedPack& pack, int parallelism);
        // Main thread: the six face textures of one decoded layer.
        bool UploadDecodedLayer(DecodedLayer& layer);
        // Main thread: uploads what is still pending, then makes the pack
        // the resident one (m_pack). False when no layer could be read.
        bool InstallDecodedPack(DecodedPack& pack);
        // The pack root a skybox id names, or empty when the id is not an
        // OptiFine pack (vanilla resolves to the enabled resource pack's sky).
        static std::string PackDirFor(const std::string& id);

        struct Prefetch {
            std::shared_ptr<DecodedPack> pack;   // decoding, or decoded and uploading
            std::future<void>            decode; // valid while the background decode runs
        };
        Prefetch m_prefetch;
        // Decodes abandoned for another sky finish on their own (a layer or
        // so after the cancel) and are joined here, never waited on.
        std::vector<std::future<void>> m_abandonedDecodes;
        bool PrefetchMatches(const std::string& id, const std::string& dir) const {
            return m_prefetch.pack && m_prefetch.pack->id == id && m_prefetch.pack->dir == dir;
        }
        void DiscardPrefetch();
        void ReapAbandonedDecodes();
        // Draws one world's layers over the sky already drawn (after the
        // sunrise glow, before the sun and moon, where OptiFine draws
        // them), each with fade × position × weather brightness for this
        // moment and its rotation for the current sky angle.
        void RenderOptiFineLayers(std::vector<OptiFineLayer>& layers, const glm::mat4& viewProj,
                                  const EnvironmentFrame& env);
        glm::ivec3 m_observerBlock{0};
        uint16_t   m_observerBiome = 0;

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
        Mesh m_twilightStars;   // TFSkyRenderer's ~3000 from the same seed

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
        // The Hush's frozen night is the active sky (see RenderFixedNight).
        bool        m_fixedNight = false;
        // The mod dimensions' skies are the active sky: the Twilight
        // Forest's (RenderTwilight) and the Aether's (the vanilla pass with
        // its frame — AetherSkyRenderEffects' colours, sun/moon fade and no
        // dark disc — and never a chosen skybox or pack).
        bool        m_twilightSky = false;
        bool        m_aetherSky = false;

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
