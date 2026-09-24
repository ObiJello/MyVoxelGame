// File: src/client/renderer/gui/screens/PanoramaRenderer.hpp
//
// The slowly-spinning title-screen skybox. Mirrors MC's CubeMap +
// PanoramaRenderer pair: six 2D textures (panorama_0..5) drawn on the
// inside faces of a unit cube around the camera, rendered with an 85°
// projection, fixed 10° downward pitch, and a yaw that advances at
// 2°/sec × the accessibility "Panorama Scroll Speed" option.
//
// Every panorama Minecraft has ever shipped lives in a versioned
// subfolder: assets/textures/gui/title/background/<slug>/panorama_0..5.png
// (slugs in kSets, chronological). The active set is the "panoramaSet"
// settings key; the Accessibility screen cycles it live via LoadSet().
#pragma once

#include "../../backend/RenderTypes.hpp"

#include "../../texture/MipmapGenerator.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    class PanoramaRenderer {
    public:
        struct SetInfo {
            const char* slug;    // asset subfolder name
            const char* label;   // display name for the options cycle button
        };

        // Settings default — the newest panorama we ship.
        static constexpr const char* kDefaultSet = "26.1";
        // Sentinel settings value: LoadSet resolves it to a random available
        // set, so a fresh one is rolled every time the title screen comes up.
        static constexpr const char* kRandomSet = "random";
        // The six faces captured from the player's eye as they last left a
        // world (PlatformMain's leave capture), kept under the game
        // directory rather than the assets: panorama/last_world/. Offered
        // as "Last World"; falls back to the default set until one exists.
        static constexpr const char* kLastWorldSet = "last_world";
        static std::string LastWorldDir();
        // The six faces (RGBA8, rows top to bottom, `size` square, in the
        // panorama_N order).
        struct LastWorldFaces {
            std::array<std::vector<uint8_t>, 6> rgba;
            int size = 0;
            // Which world, and the world yaw face 0 looks along — what the
            // join transition needs to put the camera where the panorama
            // was looking. Saved beside the faces (meta.json).
            std::string worldId;
            float       baseYaw = 0.0f;
            // Where the player was looking when they clicked to leave —
            // before the outro turn — as world yaw and pitch. The join
            // transition turns the view back to it.
            float       leaveYaw   = 0.0f;
            float       leavePitch = 0.0f;
        };
        // The captured set's world, base yaw and leave look: from memory
        // right after a capture, else from meta.json. Empty id = no capture
        // on disk.
        static std::string LastWorldId();
        static float       LastWorldBaseYaw();
        static float       LastWorldLeaveYaw();
        static float       LastWorldLeavePitch();
        // Writes them as that set. False if any write failed. Synchronous —
        // six PNG encodes of a window-sized face take a second or two, so
        // the leave path uses SaveLastWorldAsync instead.
        static bool SaveLastWorld(const LastWorldFaces& faces);
        // The leave path: the faces are kept for the next LoadSet of the
        // captured set, which builds its textures straight from them (no
        // disk round trip, no wait), while a background thread writes the
        // PNGs for later launches. FinishPendingSaves joins that thread —
        // Shutdown calls it, so the game never exits mid-write.
        static void SaveLastWorldAsync(std::shared_ptr<LastWorldFaces> faces);
        static void FinishPendingSaves();
        // The captured set's GPU textures, built DURING the capture — each
        // face's texture is made from its pixels the moment its tiles are
        // assembled — so the title has nothing to upload: six faces at 2×
        // are ~200 MB, and pushing that through the title's first frame was
        // a visible hitch in the panorama's motion. TryLoadSet adopts them.
        // `faces->rgba[face]` must already hold the face (the buffer is
        // shared with the mip job and the PNG writer, so nothing is copied).
        // Level 0 goes up now; the half-size mip is filtered on a worker
        // and uploaded by PumpLastWorldMips on a later frame, so this costs
        // one upload per face and no CPU filtering on the frame.
        static void SetLastWorldFace(int face, std::shared_ptr<LastWorldFaces> faces);
        // The streamed form of the above, so no frame carries a whole face:
        // Begin makes the texture (two levels, empty), each Region is one
        // tile's pixels uploaded at once, Finish starts the mip job once
        // `faces->rgba[face]` holds the assembled face.
        static void BeginLastWorldFace(int face, int size);
        static void UploadLastWorldRegion(int face, int x, int y, int w, int h, const uint8_t* rgba);
        static void FinishLastWorldFace(int face, std::shared_ptr<LastWorldFaces> faces);
        // Once a frame while a capture or its title is showing: uploads at
        // most one finished mip level and switches that face to trilinear.
        static void PumpLastWorldMips();
        static void DiscardAdoptedFaces();   // a capture that did not complete
        // Continuity from the world: the view the player left with, so the
        // captured set opens on exactly it. The panorama's yaw picks up
        // `yawOffsetDeg` past face 0 (MC sign, turning right positive) and
        // keeps turning; its pitch and field of view ease from the player's
        // to the title's own over a few seconds. Consumed by the next
        // LoadSet of the captured set; loading any other set drops it.
        static void SetHandoff(float yawOffsetDeg, float pitchDeg, float fovDeg);
        // True while the loaded set is continuing a world's view (its pitch
        // and field of view still easing in).
        bool HandoffActive() const { return m_handoffActive; }
        // The view as last drawn — the join transition reads the camera it
        // hands to the world off these. Yaw is past face 0, turning right
        // positive (MC sign), so world yaw = base yaw + this.
        float CurrentYawOffset() const { return -m_spin; }
        float CurrentPitch() const     { return m_lastPitch; }
        // The join transition's landing: from wherever the drift has taken
        // the view, turn (shortest way round), tilt and zoom smoothly to the
        // given view over `seconds`, then hold there — the drift stops. The
        // world takes over at exactly this view when EaseDone.
        void EaseTo(float yawOffsetDeg, float pitchDeg, float fovDeg, float seconds);
        bool EaseDone() const { return m_easeDone; }

        // The known sets (chronological) filtered down to the ones whose
        // asset folders actually exist on disk, then "Last World".
        static std::vector<SetInfo> AvailableSets();

        bool Initialize(const std::string& setSlug = kDefaultSet);
        void Shutdown();

        // Swap the skybox to another set at runtime (options screen).
        // Falls back through kDefaultSet to a gradient if files are missing.
        void LoadSet(const std::string& slug);
        const std::string& CurrentSet() const { return m_currentSet; }

        // Advance the spin and draw the skybox to the current framebuffer.
        // Call before the GUI pass. `speed` is the panorama scroll-speed
        // option (1.0 = vanilla).
        void Render(int fbWidth, int fbHeight, float deltaSeconds, float speed);

        // Full-screen vignette overlay (panorama_overlay.png) — drawn in GUI
        // space by the title screen AFTER the skybox, with fade alpha.
        void RenderOverlay(GuiGraphics& g, int guiWidth, int guiHeight, float alpha);

        bool HasPanoramaTextures() const { return m_texturesValid; }

    private:
        ShaderHandle  m_shader = INVALID_SHADER;
        BufferHandle  m_vb     = INVALID_BUFFER;
        BufferHandle  m_ib     = INVALID_BUFFER;
        MeshHandle    m_mesh   = INVALID_MESH;
        TextureHandle m_faces[6] = {INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
                                    INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE};
        TextureHandle m_overlay = INVALID_TEXTURE;
        std::string m_currentSet;
        bool  m_texturesValid = false;
        bool  m_initialized   = false;
        float m_spin          = 0.0f;   // degrees; grows = the view turns LEFT

        // SetHandoff — pending until a LoadSet consumes it.
        static std::shared_ptr<LastWorldFaces> s_memoryFaces;   // SaveLastWorldAsync -> TryLoadSet (fallback)
        static TextureHandle s_adoptedFaces[6];                  // SetLastWorldFace -> TryLoadSet
        static int           s_adoptedSize;
        // The captured faces' textures wherever they currently live
        // (adopted or not), for the mip uploads; cleared as they die.
        static TextureHandle s_lastWorldTex[6];
        static std::future<Mipmap::Image> s_mipJobs[6];
        static bool          s_mipUploaded[6];
        static std::thread s_saveThread;
        static bool  s_handoffPending;
        static float s_handoffYaw, s_handoffPitch, s_handoffFov;
        // The easing in progress (the captured set, just loaded).
        bool  m_handoffActive = false;
        float m_pitchFrom = 10.0f, m_fovFrom = 85.0f;
        float m_lastPitch = 10.0f, m_lastFov = 85.0f;   // as last drawn
        // EaseTo state.
        bool  m_easeActive = false, m_easeDone = false;
        float m_easeSeconds = 1.0f;
        float m_spinFrom = 0.0f, m_spinTo = 0.0f;
        float m_easePitchFrom = 10.0f, m_easePitchTo = 10.0f;
        float m_easeFovFrom = 85.0f, m_easeFovTo = 85.0f;
        std::chrono::steady_clock::time_point m_easeStart{};
        static std::string s_lastWorldId;   // meta cache; "" = not read yet
        static float       s_lastWorldBaseYaw;
        static float       s_lastWorldLeaveYaw;
        static float       s_lastWorldLeavePitch;
        static bool        s_lastWorldMetaRead;
        static void        EnsureLastWorldMeta();
        std::chrono::steady_clock::time_point m_handoffStart{};
        void ApplyHandoff(const std::string& loadedSlug);

        void DestroyFaceTextures();
        bool TryLoadSet(const std::string& slug);

        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;
    };

    extern PanoramaRenderer g_panoramaRenderer;

} // namespace Render
